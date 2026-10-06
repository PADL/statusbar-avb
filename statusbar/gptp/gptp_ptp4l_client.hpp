// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

/// Ptp4lClient — the live gPTP state of a running linuxptp `ptp4l`, read
/// over its management socket.
///
/// linuxptp answers IEEE 1588-2019 Clause 15 management GETs on a Unix
/// datagram socket (`uds_address`, root-only by default) and, since
/// linuxptp 3.1, on a read-only one (`uds_ro_address`, usually
/// /var/run/ptp4lro, world-readable) that accepts GET only. This client
/// speaks the GET half of that protocol — the same bytes `pmc -u` sends —
/// and keeps the answers as one Ptp4lStatus snapshot: the port's own
/// identity, priorities and intervals (DEFAULT_DATA_SET, PORT_DATA_SET),
/// the grandmaster it follows (PARENT_DATA_SET, TIME_STATUS_NP), the
/// measured peer delay (PORT_DATA_SET) and asCapable (PORT_DATA_SET_NP).
/// That is everything an ATDECC entity needs to fill its AVB_INTERFACE
/// descriptor and answer GET_AVB_INFO truthfully.
///
/// It is a net::Pollable: tick() sends the six GETs every poll interval,
/// on_ready() parses the replies. No ptp4l (or no permission) just leaves
/// the snapshot invalid; the entity falls back to what it observes on the
/// wire. The message codec is exposed for tests.
#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace statusbar::gptp {

/// IEEE 1588-2019 Table 59 management ids (plus linuxptp's implementation
/// specific ones) this client understands.
namespace ptp4l_management_id {
constexpr uint16_t DEFAULT_DATA_SET = 0x2000;
constexpr uint16_t CURRENT_DATA_SET = 0x2001;
constexpr uint16_t PARENT_DATA_SET = 0x2002;
constexpr uint16_t PORT_DATA_SET = 0x2004;
constexpr uint16_t TIME_STATUS_NP = 0xC000;    ///< linuxptp
constexpr uint16_t PORT_DATA_SET_NP = 0xC002;  ///< linuxptp
}  // namespace ptp4l_management_id

/// IEEE 1588-2019 Table 35 portState values.
namespace ptp4l_port_state {
constexpr uint8_t INITIALIZING = 1;
constexpr uint8_t FAULTY = 2;
constexpr uint8_t DISABLED = 3;
constexpr uint8_t LISTENING = 4;
constexpr uint8_t PRE_MASTER = 5;
constexpr uint8_t MASTER = 6;
constexpr uint8_t PASSIVE = 7;
constexpr uint8_t UNCALIBRATED = 8;
constexpr uint8_t SLAVE = 9;
}  // namespace ptp4l_port_state

/// One snapshot of the data sets ptp4l reports. Every field is host order;
/// TimeIntervals are reduced to whole nanoseconds.
struct Ptp4lStatus
{
    // ---- DEFAULT_DATA_SET: this clock ----
    ClockIdentity clock_identity{};
    uint8_t priority1{0};
    uint8_t priority2{0};
    uint8_t clock_class{0};
    uint8_t clock_accuracy{0};
    uint16_t offset_scaled_log_variance{0};
    uint8_t domain_number{0};
    uint16_t number_ports{0};
    bool have_default{false};

    // ---- CURRENT_DATA_SET ----
    uint16_t steps_removed{0};
    int64_t offset_from_master_ns{0};
    int64_t mean_path_delay_ns{0};
    bool have_current{false};

    // ---- PARENT_DATA_SET: the grandmaster ----
    ClockIdentity grandmaster_identity{};
    uint8_t gm_priority1{0};
    uint8_t gm_priority2{0};
    uint8_t gm_clock_class{0};
    uint8_t gm_clock_accuracy{0};
    uint16_t gm_offset_scaled_log_variance{0};
    bool have_parent{false};

    // ---- PORT_DATA_SET (first port) ----
    uint16_t port_number{0};
    uint8_t port_state{0};
    int64_t peer_mean_path_delay_ns{0};
    int8_t log_announce_interval{0};
    uint8_t announce_receipt_timeout{0};
    int8_t log_sync_interval{0};
    uint8_t delay_mechanism{0};
    int8_t log_min_pdelay_req_interval{0};
    uint8_t version_number{0};
    bool have_port{false};

    // ---- PORT_DATA_SET_NP (linuxptp) ----
    bool as_capable{false};
    uint32_t neighbor_prop_delay_thresh{0};
    bool have_port_np{false};

    // ---- TIME_STATUS_NP (linuxptp) ----
    int64_t master_offset_ns{0};
    bool gm_present{false};
    uint16_t gm_time_base_indicator{0};
    bool have_time_status{false};

    int64_t sampled_ns{0};  ///< reactor time of the newest reply

    /// True once the clock's own data set has been read; the other groups
    /// carry their own have_* flags.
    [[nodiscard]] auto valid() const noexcept -> bool { return have_default; }
};

class Ptp4lClient : public net::Pollable
{
  public:
    struct Config
    {
        /// ptp4l's read-only management socket (uds_ro_address).
        std::string uds_path = "/var/run/ptp4lro";
        /// Where this client binds its own datagram socket; the pid and a
        /// counter are appended so several clients in one process coexist.
        std::string local_path_prefix = "/tmp/statusbar-ptp4l-client";
        uint8_t domain_number = 0;       ///< ptp4l answers only its own domain
        uint8_t transport_specific = 0;  ///< the majorSdoId nibble (gPTP profiles use 1)
        int64_t poll_interval_ns = 1'000'000'000;
        /// Replies older than this many intervals mark the snapshot stale
        /// (valid() goes false) — ptp4l stopped or lost its socket.
        int stale_after_intervals = 3;
    };

    using ChangedFn = statusbar::sg14::inplace_function<void(Ptp4lStatus const&), 64>;

    explicit Ptp4lClient(Config config);
    ~Ptp4lClient() override;
    Ptp4lClient(Ptp4lClient const&) = delete;
    auto operator=(Ptp4lClient const&) -> Ptp4lClient& = delete;
    Ptp4lClient(Ptp4lClient&&) = delete;
    auto operator=(Ptp4lClient&&) -> Ptp4lClient& = delete;

    /// Create and bind the local socket. Fails when the local path cannot
    /// be bound; whether ptp4l is actually there only shows in the status.
    [[nodiscard]] auto open() -> Status;
    [[nodiscard]] auto is_open() const noexcept -> bool { return fd_ >= 0; }

    [[nodiscard]] auto config() const noexcept -> Config const& { return config_; }
    [[nodiscard]] auto status() const noexcept -> Ptp4lStatus const& { return status_; }

    /// Fires after a reply batch changed anything an ATDECC entity exposes
    /// (grandmaster, asCapable, port state, priorities, domain, peer delay).
    void set_on_changed(ChangedFn fn) { on_changed_ = std::move(fn); }

    /// Send the GET batch now (tick() does this every poll interval).
    void request_now(int64_t now_ns);

    // -- net::Pollable --
    [[nodiscard]] auto fd() const noexcept -> int override { return fd_; }
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

    // -- codec (exposed for tests) --

    /// Encode one management GET for @p management_id into @p out; returns
    /// the frame length (0 if @p out is too small).
    [[nodiscard]] static auto encode_get(
        uint16_t management_id, uint16_t sequence_id, uint8_t domain_number, uint8_t transport_specific, std::span<uint8_t> out)
        -> size_t;

    /// Parse one management RESPONSE frame into @p status. Returns the
    /// management id handled, or 0 when the frame is not a usable response
    /// (wrong type, an error status TLV, or a short data field).
    [[nodiscard]] static auto decode_response(std::span<uint8_t const> frame, Ptp4lStatus& status, int64_t now_ns) -> uint16_t;

    /// The management ids request_now() asks for, in order.
    static constexpr std::array<uint16_t, 6> REQUESTED_IDS{
        ptp4l_management_id::DEFAULT_DATA_SET,
        ptp4l_management_id::CURRENT_DATA_SET,
        ptp4l_management_id::PARENT_DATA_SET,
        ptp4l_management_id::PORT_DATA_SET,
        ptp4l_management_id::PORT_DATA_SET_NP,
        ptp4l_management_id::TIME_STATUS_NP,
    };

  private:
    [[nodiscard]] static auto exposed_fields_changed(Ptp4lStatus const& a, Ptp4lStatus const& b) noexcept -> bool;

    Config config_;
    int fd_{-1};
    std::string local_path_;
    uint16_t sequence_id_{0};
    int64_t last_request_ns_{0};
    int64_t last_reply_ns_{0};
    Ptp4lStatus status_{};
    Ptp4lStatus announced_{};  ///< what on_changed_ last saw
    bool batch_dirty_{false};
    ChangedFn on_changed_{};
    std::array<uint8_t, 1500> rx_buf_{};
};

}  // namespace statusbar::gptp
