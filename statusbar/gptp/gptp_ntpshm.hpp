#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file gptp_ntpshm.hpp
/// @brief Linux-only NTP shared memory interface for PTP time access.
///
/// Reads PTP-to-CLOCK_REALTIME offset from linuxptp's ntpshm clock servo via
/// a System V shared memory segment. The shared memory layout (NtpShmSegment)
/// follows the NTP SHM reference clock protocol:
///
///   - @c mode:  always 1 (producer/consumer with torn-read detection)
///   - @c count: volatile sequence counter (odd = write in progress)
///   - @c clock_timestamp_sec / clock_timestamp_nsec: PTP (reference) time
///   - @c receive_timestamp_sec / receive_timestamp_nsec: local CLOCK_REALTIME
///   - @c valid: non-zero when the sample is ready to read
///
/// Usage example:
/// @code
///   // Direct reader
///   auto reader = NtpShmReader::open(0);  // attach to NTP0
///   if (reader) {
///       auto sample = reader->read_sample();
///       if (sample) {
///           int64_t ptp_ns = sample->ptp_ns;
///       }
///   }
/// @endcode
///
/// To use NTP SHM as a PTP clock source in the create_client / setup_ptp
/// pipeline, see @c ptpclient::NtpShmPtpClient in
/// @c statusbar/ptpclient/ptpclient_ntpshm.hpp, which adapts this reader to the
/// PtpClientBase interface.
///
/// @see https://docs.ntpsec.org/latest/driver_shm.html

#if defined(__linux__)

#    include "statusbar/sg14/inplace_function.h"
#    include "statusbar/status/status.hpp"

#    include <cstdint>
#    include <ctime>
#    include <memory>

#    include <sys/shm.h>

namespace statusbar::gptp {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

/// NTP SHM segment key base (NTP0 = 0x4E545030)
inline constexpr int NTPSHM_KEY_BASE = 0x4E545030;

inline constexpr int64_t NS_PER_SEC = 1'000'000'000LL;

/// NTP shared memory segment layout (mode 1).
///
/// This struct matches the binary layout used by ntpd, chrony, and linuxptp.
/// The producer (linuxptp) writes samples atomically using a sequence-count
/// protocol: it sets @c count to an odd value before writing, then increments
/// @c count to an even value and sets @c valid after the write completes.
/// The consumer detects torn reads by comparing @c count before and after.
///
/// @note Fields ending in @c _usec are legacy (mode 0); mode 1 uses the
///       @c _nsec variants for nanosecond precision.
struct NtpShmSegment
{
    int mode;                      ///< Protocol mode (1 = sequence-count torn-read detection)
    int volatile count;            ///< Sequence counter; odd while producer is writing
    time_t clock_timestamp_sec;    ///< PTP (reference clock) time, seconds
    int clock_timestamp_usec;      ///< PTP time, microseconds (mode 0 only, unused in mode 1)
    time_t receive_timestamp_sec;  ///< Local CLOCK_REALTIME, seconds
    int receive_timestamp_usec;    ///< Local time, microseconds (mode 0 only, unused in mode 1)
    int leap;                      ///< Leap-second indicator
    int precision;                 ///< Clock precision (log2 seconds)
    int nsamples;                  ///< Number of samples (informational)
    int volatile valid;            ///< Non-zero when a consistent sample is available
    int clock_timestamp_nsec;      ///< PTP (reference clock) time, nanoseconds (mode 1)
    int receive_timestamp_nsec;    ///< Local CLOCK_REALTIME, nanoseconds (mode 1)
};

/// A single PTP/local time sample pair read from the NTP shared memory segment.
///
/// Captures both the PTP reference clock time and the corresponding local
/// CLOCK_REALTIME at the moment the PTP daemon recorded the sample.
struct NtpShmSample
{
    int64_t ptp_ns{0};    ///< PTP (reference clock) timestamp in nanoseconds
    int64_t local_ns{0};  ///< Local CLOCK_REALTIME timestamp in nanoseconds
};

/// Error codes for NTP SHM operations
enum class NtpShmError
{
    attach_failed = 1,
    no_valid_sample,
};

/// Error category for NtpShmError
class NtpShmErrorCategory : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.gptp.ntpshm"; }

    [[nodiscard]] auto message(int ev) const -> std::string override;
};

inline auto ntpshm_error_category() -> NtpShmErrorCategory const&
{
    static NtpShmErrorCategory const instance;
    return instance;
}

inline auto make_error_code(NtpShmError e) -> std::error_code
{
    return {static_cast<int>(e), ntpshm_error_category()};
}

/// RAII reader for the NTP shared memory segment.
///
/// Attaches to a System V shared memory segment on construction (via the
/// static @c open() factory) and detaches on destruction. The segment is
/// attached read-only with @c SHM_RDONLY.
///
/// This class is move-only; copying is deleted because each instance owns
/// its @c shmat() attachment.
class NtpShmReader
{
  public:
    /// Attach to NTP SHM segment (read-only)
    /// @param segment Segment index (0 = NTP0, 1 = NTP1, etc.)
    [[nodiscard]] static auto open(int segment = 0) noexcept -> StatusValue<NtpShmReader>;

    ~NtpShmReader() noexcept
    {
        if (shm_ != nullptr) {
            ::shmdt(shm_);
        }
    }

    // Move only
    NtpShmReader(NtpShmReader&& other) noexcept
        : shm_(other.shm_)
    {
        other.shm_ = nullptr;
    }

    auto operator=(NtpShmReader&& other) noexcept -> NtpShmReader&;

    NtpShmReader(NtpShmReader const&) = delete;
    auto operator=(NtpShmReader const&) -> NtpShmReader& = delete;

    /// Read a consistent sample using the mode-1 torn-read detection protocol.
    ///
    /// Implements the NTP SHM consumer side of the sequence-count protocol:
    ///  1. Read @c count (must be even, meaning no write in progress).
    ///  2. Check @c valid is non-zero.
    ///  3. Copy the timestamp fields.
    ///  4. Re-read @c count; if it changed, the sample was torn -- retry.
    ///
    /// Retries up to 10 times before returning @c NtpShmError::no_valid_sample.
    ///
    /// @return A consistent NtpShmSample on success, or an error if no valid
    ///         sample could be read after all retry attempts.
    [[nodiscard]] auto read_sample() const noexcept -> StatusValue<NtpShmSample>;

    /// Get current PTP time by applying the SHM offset to CLOCK_REALTIME
    /// @return PTP time in nanoseconds
    [[nodiscard]] auto get_ptp_time_ns() const noexcept -> StatusValue<int64_t>;

    /// Get a PtpTimeReader-compatible callable for use with PtpTimeBridge
    /// @return A function that returns StatusValue<int64_t> with PTP time in nanoseconds
    [[nodiscard]] auto reader() const -> statusbar::sg14::inplace_function<StatusValue<int64_t>(), 64>
    {
        return [this]() -> StatusValue<int64_t> { return get_ptp_time_ns(); };
    }

  private:
    explicit NtpShmReader(NtpShmSegment const* shm) noexcept
        : shm_(shm)
    {}

    NtpShmSegment const* shm_{nullptr};
};

}  // namespace statusbar::gptp

template <>
struct std::is_error_code_enum<statusbar::gptp::NtpShmError> : std::true_type
{};

// NOTE: the PtpClientBase adapter for this reader (NtpShmPtpClient) lives in the
// ptpclient module (statusbar/ptpclient/ptpclient_ntpshm.hpp), not here, so the
// gptp module never depends on ptpclient. gptp owns only the raw SHM reader.

#endif  // __linux__
