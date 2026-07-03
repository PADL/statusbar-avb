// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// gps_ratio_tracker: sample the local PHC (switch gPTP time) against GPS
// (CLOCK_REALTIME, chrony-disciplined to the site GPS grandmaster) and track
// r = switch_rate / GPS_rate using the ptpclient frequency-ratio estimators.
// Feeds the GPS-rate media-clock generator. See avb/docs/GPS_MEDIA_CLOCK.md.
//
// Linux-only (reads /dev/ptp0 via its dynamic POSIX clock-id, bracketed by
// CLOCK_REALTIME). Pick the estimator with --filter ols|kalman.
//
//   sudo statusbar-gps-ratio-tracker --device /dev/ptp0 --filter kalman
//   sudo statusbar-gps-ratio-tracker --filter ols --window-s 900

#include "statusbar/ptpclient/ptpclient_freq_ratio.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

using statusbar::ptpclient::KalmanRatioTracker;
using statusbar::ptpclient::OlsRatioTracker;
using statusbar::ptpclient::RatioEstimate;

namespace {

clockid_t fd_to_clockid(int fd)
{
    return static_cast<clockid_t>((~static_cast<unsigned>(fd) << 3) | 3u);
}
std::int64_t to_i64(timespec const& t)
{
    return (static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000LL) + t.tv_nsec;
}

struct Reading
{
    std::int64_t offset_ns;  // PHC - GPS
    timespec gps;            // CLOCK_REALTIME bracket midpoint (for dt)
};

// Bracket the PHC read with two CLOCK_REALTIME reads; midpoint REALTIME so PHC
// and GPS are aligned to within the (~us) bracket window.
std::optional<Reading> read_sample(int fd, clockid_t phc_clk)
{
    timespec g1{}, p{}, g2{};
    clock_gettime(CLOCK_REALTIME, &g1);
    if (clock_gettime(phc_clk, &p) != 0) {
        return std::nullopt;
    }
    clock_gettime(CLOCK_REALTIME, &g2);
    std::int64_t const mid = (to_i64(g1) + to_i64(g2)) / 2;
    timespec gps{.tv_sec = static_cast<time_t>(mid / 1'000'000'000LL), .tv_nsec = static_cast<long>(mid % 1'000'000'000LL)};
    return Reading{.offset_ns = to_i64(p) - mid, .gps = gps};
}

double dt_seconds(timespec const& now, timespec const& prev)
{
    return static_cast<double>(now.tv_sec - prev.tv_sec) + (static_cast<double>(now.tv_nsec - prev.tv_nsec) * 1e-9);
}

}  // namespace

int main(int argc, char** argv)
{
    std::string dev = "/dev/ptp0";
    std::string filter = "kalman";
    double report_s = 5.0, sample_hz = 4.0, fs = 96000.0, window_s = 900.0;
    KalmanRatioTracker::Config kc;

    for (int i = 1; i < argc - 1; ++i) {
        std::string const a = argv[i];
        if (a == "--device") {
            dev = argv[++i];
        } else if (a == "--filter") {
            filter = argv[++i];
        } else if (a == "--report-s") {
            report_s = atof(argv[++i]);
        } else if (a == "--sample-hz") {
            sample_hz = atof(argv[++i]);
        } else if (a == "--fs") {
            fs = atof(argv[++i]);
        } else if (a == "--window-s") {
            window_s = atof(argv[++i]);
        } else if (a == "--meas-noise-ns") {
            kc.meas_noise_ns = atof(argv[++i]);
        } else if (a == "--jerk-psd") {
            kc.jerk_psd = atof(argv[++i]);
        } else if (a == "--gate") {
            kc.gate_sigmas = atof(argv[++i]);
        }
    }

    // sample_hz drives 1e9/sample_hz (sleep) and 1.0/sample_hz (report accumulator);
    // a non-positive value yields inf/an invalid tv_nsec, so the sleep returns EINVAL
    // and the loop spins. Reject it up front.
    if (sample_hz <= 0.0) {
        std::fprintf(stderr, "Error: --sample-hz must be positive\n");
        return 1;
    }

    int const fd = open(dev.c_str(), O_RDWR);
    if (fd < 0) {
        perror(("open " + dev).c_str());
        return 1;
    }
    clockid_t const phc_clk = fd_to_clockid(fd);
    bool const use_kalman = (filter != "ols");

    OlsRatioTracker ols{window_s};
    KalmanRatioTracker kf{kc};

    std::optional<timespec> prev_gps;
    double since_report = 0;
    bool first = true;

    for (;;) {
        if (auto s = read_sample(fd, phc_clk)) {
            double const dt = prev_gps ? dt_seconds(s->gps, *prev_gps) : 0.0;
            prev_gps = s->gps;
            if (use_kalman) {
                kf.add(s->offset_ns, dt);
            } else {
                ols.add(s->offset_ns, dt);
            }
            since_report += 1.0 / sample_hz;
            if (first || since_report >= report_s) {
                first = false;
                since_report = 0;
                RatioEstimate const e = use_kalman ? kf.estimate() : ols.estimate();
                if (e.valid) {
                    printf(
                        "[%s] r=%.9f (%+0.4f ppm)  f_unc=%.3f ppb  drift=%+0.4f ppm/hr  "
                        "PHC-GPS=%lld ns  avtp_ns/sample@%.0f=%.4f\n",
                        use_kalman ? "kalman" : "ols",
                        e.r,
                        e.ppm(),
                        e.freq_uncertainty_ppb,
                        e.drift_ppm_per_hr,
                        static_cast<long long>(e.offset_ns),
                        fs,
                        e.phc_ns_per_sample(fs));
                    fflush(stdout);
                }
            }
        }
        timespec ts{.tv_sec = 0, .tv_nsec = static_cast<long>(1e9 / sample_hz)};
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
    }
}
