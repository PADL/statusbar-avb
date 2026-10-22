#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP management over ptp4l's UNIX-domain socket, pmc-style (IEEE 1588-2019 Clause 15): which
/// grandmaster the local clock follows. The domain must be ptp4l's. Linux only; elsewhere
/// create() fails with not_supported. See docs/LINUXPTP.md.

#include "statusbar/buffer/file_descriptor.hpp"
#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <variant>

namespace statusbar::ptpclient {

/// IEEE 1588-2019 Table 59 managementId values used here.
namespace management_id {
inline constexpr uint16_t default_data_set = 0x2000;
inline constexpr uint16_t current_data_set = 0x2001;
inline constexpr uint16_t parent_data_set = 0x2002;
inline constexpr uint16_t time_properties_data_set = 0x2003;
}  // namespace management_id

/// actionField (Table 57).
enum class ManagementAction : uint8_t
{
    get = 0,
    set = 1,
    response = 2,
    command = 3,
    acknowledge = 4,
};

/// PARENT_DATA_SET (15.5.3.3.1).
struct ParentDataSet
{
    gptp::SourcePortIdentity parent_port_identity{};
    bool parent_stats{false};
    uint16_t observed_parent_offset_scaled_log_variance{0};
    int32_t observed_parent_clock_phase_change_rate{0};
    uint8_t grandmaster_priority1{0};
    uint8_t grandmaster_clock_class{0};
    uint8_t grandmaster_clock_accuracy{0};
    uint16_t grandmaster_offset_scaled_log_variance{0};
    uint8_t grandmaster_priority2{0};
    ieee::Eui64 grandmaster_identity{};

    static constexpr size_t SIZE = 32;
};

/// CURRENT_DATA_SET (15.5.3.2.1). TimeIntervals are ns scaled by 2^16.
struct CurrentDataSet
{
    uint16_t steps_removed{0};
    int64_t offset_from_master_scaled{0};
    int64_t mean_path_delay_scaled{0};

    static constexpr size_t SIZE = 18;
};

/// TIME_PROPERTIES_DATA_SET (15.5.3.4.1).
struct TimePropertiesDataSet
{
    int16_t current_utc_offset{0};
    uint8_t flags{0};  ///< leap61, leap59, utcOffsetValid, ptpTimescale, timeTraceable, frequencyTraceable (bits 0..5)
    uint8_t time_source{0};

    static constexpr size_t SIZE = 4;
};

/// One parsed management message.
struct ManagementMessage
{
    uint8_t domain{0};
    uint16_t sequence_id{0};
    gptp::SourcePortIdentity source{};
    ManagementAction action{ManagementAction::get};
    uint16_t management_id{0};
    /// A MANAGEMENT_ERROR_STATUS TLV's managementErrorId, instead of data.
    std::optional<uint16_t> error;
    std::variant<std::monostate, ParentDataSet, CurrentDataSet, TimePropertiesDataSet> data;
};

/// Bytes of a GET with no data: header 34 + management header 14 + TLV header 6.
inline constexpr size_t MANAGEMENT_GET_SIZE = 54;

/// Write a GET for `management_id` into `out` (at least MANAGEMENT_GET_SIZE
/// bytes) addressed to every clock and port, as pmc does: returns the bytes
/// written, or 0 if `out` is too small.
[[nodiscard]] auto build_management_get(
    std::span<uint8_t> out,
    uint16_t management_id,
    uint8_t domain,
    uint16_t sequence_id,
    gptp::SourcePortIdentity const& source) noexcept -> size_t;

/// Parse a management message. Fails with bad_message unless it is a well-formed PTPv2 management
/// message with one MANAGEMENT or MANAGEMENT_ERROR_STATUS TLV long enough for its data set; other
/// data sets parse with `data` empty.
[[nodiscard]] auto parse_management_message(std::span<uint8_t const> bytes) noexcept -> StatusValue<ManagementMessage>;

/// What the client knows of ptp4l.
enum class PtpManagementStatus : uint8_t
{
    unreachable,     ///< no answer in the configured domain (ptp4l absent, refused, or in another domain)
    no_grandmaster,  ///< ptp4l answers, and its PARENT_DATA_SET names its own clock: it follows no other
    locked,          ///< ptp4l answers, following `grandmaster`
};

/// What the client has learnt.
struct PtpManagementState
{
    PtpManagementStatus status{PtpManagementStatus::unreachable};
    /// PARENT_DATA_SET's grandmasterIdentity while ptp4l answers (its own
    /// clock when no_grandmaster: then it is, or is becoming, the grandmaster).
    std::optional<ieee::Eui64> grandmaster;
    std::optional<ParentDataSet> parent;
    std::optional<CurrentDataSet> current;
    std::optional<TimePropertiesDataSet> time_properties;
    /// The first send or receive error since the last answer (EACCES on a
    /// root-only socket, ENOENT with no ptp4l, ...); clear while ptp4l answers.
    std::error_code error;
};

struct PtpManagementConfig
{
    std::string uds_address{"/var/run/ptp4l"};  ///< ptp4l's uds_address; absolute
    uint8_t domain{0};                          ///< ptp4l's domainNumber (pmc -d)
    int64_t period_ns{2'000'000'000};           ///< between queries
    int missed_periods{3};                      ///< periods without an answer before unreachable; at least 1
};

class PtpManagementClient final : public net::Pollable
{
  public:
    /// Called on the reactor thread when the status, the grandmaster or the
    /// error changes.
    using Listener = sg14::inplace_function<void(PtpManagementState const&), 64>;

    /// Bind the client's socket. Fails with invalid_argument for a bad
    /// configuration, not_supported off Linux, or the socket's error.
    [[nodiscard]] static auto create(PtpManagementConfig config, Listener listener)
        -> StatusValue<std::unique_ptr<PtpManagementClient>>;

    ~PtpManagementClient() override;
    PtpManagementClient(PtpManagementClient const&) = delete;
    auto operator=(PtpManagementClient const&) -> PtpManagementClient& = delete;

    [[nodiscard]] auto state() const noexcept -> PtpManagementState const& { return state_; }

    /// Take one received message (on_ready's body; public for tests).
    void handle(std::span<uint8_t const> bytes, int64_t now_ns);
    /// Send this period's GETs (tick's body when due; public for tests).
    void query(int64_t now_ns);

    // -- Pollable --
    [[nodiscard]] auto fd() const noexcept -> int override { return fd_.get(); }
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override;
    /// True after a socket error other than EAGAIN/EINTR (see state().error).
    [[nodiscard]] auto finished() const noexcept -> bool override { return finished_; }

  private:
    PtpManagementClient(
        PtpManagementConfig config,
        Listener listener,
        FileDescriptor fd,
        net::SocketAddress server,
        std::string local_path) noexcept;
    void note_error(std::error_code error);
    void publish_if_changed();

    PtpManagementConfig config_;
    Listener listener_;
    FileDescriptor fd_;
    net::SocketAddress server_;
    std::string local_path_;
    gptp::SourcePortIdentity source_{};
    uint16_t sequence_{0};
    int64_t next_query_ns_{0};
    bool queried_{false};
    bool finished_{false};
    int64_t last_answer_ns_{0};
    PtpManagementState state_;
    PtpManagementStatus reported_status_{PtpManagementStatus::unreachable};
    std::optional<ieee::Eui64> reported_grandmaster_;
    std::error_code reported_error_;
    std::array<uint8_t, 1500> buffer_{};
};

}  // namespace statusbar::ptpclient
