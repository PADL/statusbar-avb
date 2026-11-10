#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The wire layouts of PTP management messages (IEEE 1588-2019 Clause 15) and of the data sets
/// ptpclient's management client reads and writes, linuxptp's implementation-specific ones as
/// its tlv.h packs them; read and written with span_load / span_store.

#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace statusbar::ptpclient {

using ieee::doublet_t;
using ieee::Eui64;
using ieee::octet_t;
using ieee::octlet_t;
using ieee::quadlet_t;

//
// Management message - Clause 15.4.1, 15.5.2
//

/// The management header after the common header (Clause 15.4.1).
struct ManagementHeaderPayload
{
    static constexpr size_t LENGTH = 14;
    gptp::SourcePortIdentity target_port_identity;
    octet_t starting_boundary_hops;
    octet_t boundary_hops;
    octet_t action;  ///< actionField in the low nibble (Table 57)
    octet_t reserved;
};
static_assert(sizeof(ManagementHeaderPayload) == ManagementHeaderPayload::LENGTH);
static_assert(offsetof(ManagementHeaderPayload, starting_boundary_hops) == 10);
static_assert(offsetof(ManagementHeaderPayload, action) == 12);

/// A TLV's header (Clause 14.1): its type and the length of what follows.
struct TlvHeaderPayload
{
    static constexpr size_t LENGTH = 4;
    doublet_t tlv_type;
    doublet_t length_field;
};
static_assert(sizeof(TlvHeaderPayload) == TlvHeaderPayload::LENGTH);
static_assert(offsetof(TlvHeaderPayload, length_field) == 2);

/// A MANAGEMENT TLV's header and managementId (Clause 15.5.2), its data set following.
struct ManagementTlvPayload
{
    static constexpr size_t LENGTH = 6;
    doublet_t tlv_type;
    doublet_t length_field;
    doublet_t management_id;
};
static_assert(sizeof(ManagementTlvPayload) == ManagementTlvPayload::LENGTH);
static_assert(offsetof(ManagementTlvPayload, management_id) == 4);

/// A MANAGEMENT_ERROR_STATUS TLV's value up to its displayData (Clause 15.5.4).
struct ManagementErrorStatusPayload
{
    static constexpr size_t LENGTH = 8;
    doublet_t management_error_id;
    doublet_t management_id;
    quadlet_t reserved;
};
static_assert(sizeof(ManagementErrorStatusPayload) == ManagementErrorStatusPayload::LENGTH);
static_assert(offsetof(ManagementErrorStatusPayload, management_id) == 2);

//
// Data sets - Clause 15.5.3
//

/// PARENT_DATA_SET (Clause 15.5.3.3.1).
struct ParentDataSetPayload
{
    static constexpr size_t LENGTH = 32;
    gptp::SourcePortIdentity parent_port_identity;
    octet_t flags;  ///< PS in bit 0
    octet_t reserved;
    doublet_t observed_parent_offset_scaled_log_variance;
    quadlet_t observed_parent_clock_phase_change_rate;
    octet_t grandmaster_priority1;
    octet_t grandmaster_clock_class;
    octet_t grandmaster_clock_accuracy;
    doublet_t grandmaster_offset_scaled_log_variance;
    octet_t grandmaster_priority2;
    Eui64 grandmaster_identity;
};
static_assert(sizeof(ParentDataSetPayload) == ParentDataSetPayload::LENGTH);
static_assert(offsetof(ParentDataSetPayload, observed_parent_offset_scaled_log_variance) == 12);
static_assert(offsetof(ParentDataSetPayload, grandmaster_priority1) == 18);
static_assert(offsetof(ParentDataSetPayload, grandmaster_offset_scaled_log_variance) == 21);
static_assert(offsetof(ParentDataSetPayload, grandmaster_identity) == 24);

/// CURRENT_DATA_SET (Clause 15.5.3.2.1): TimeIntervals, ns scaled by 2^16.
struct CurrentDataSetPayload
{
    static constexpr size_t LENGTH = 18;
    doublet_t steps_removed;
    octlet_t offset_from_master;
    octlet_t mean_path_delay;
};
static_assert(sizeof(CurrentDataSetPayload) == CurrentDataSetPayload::LENGTH);
static_assert(offsetof(CurrentDataSetPayload, offset_from_master) == 2);
static_assert(offsetof(CurrentDataSetPayload, mean_path_delay) == 10);

/// TIME_PROPERTIES_DATA_SET (Clause 15.5.3.4.1).
struct TimePropertiesDataSetPayload
{
    static constexpr size_t LENGTH = 4;
    doublet_t current_utc_offset;
    octet_t flags;
    octet_t time_source;
};
static_assert(sizeof(TimePropertiesDataSetPayload) == TimePropertiesDataSetPayload::LENGTH);
static_assert(offsetof(TimePropertiesDataSetPayload, time_source) == 3);

/// PORT_DATA_SET (Clause 15.5.3.6.1): peerMeanPathDelay is ns scaled by 2^16.
struct PortDataSetPayload
{
    static constexpr size_t LENGTH = 26;
    gptp::SourcePortIdentity port_identity;
    octet_t port_state;
    octet_t log_min_delay_req_interval;
    octlet_t peer_mean_path_delay;
    octet_t log_announce_interval;
    octet_t announce_receipt_timeout;
    octet_t log_sync_interval;
    octet_t delay_mechanism;
    octet_t log_min_pdelay_req_interval;
    octet_t version_number;
};
static_assert(sizeof(PortDataSetPayload) == PortDataSetPayload::LENGTH);
static_assert(offsetof(PortDataSetPayload, port_state) == 10);
static_assert(offsetof(PortDataSetPayload, peer_mean_path_delay) == 12);
static_assert(offsetof(PortDataSetPayload, delay_mechanism) == 23);
static_assert(offsetof(PortDataSetPayload, version_number) == 25);

//
// linuxptp's implementation-specific data sets (tlv.h)
//

/// PORT_PROPERTIES_NP up to its interface name's PTPText lengthField; the text follows.
struct PortPropertiesNpPayload
{
    static constexpr size_t LENGTH = 13;
    gptp::SourcePortIdentity port_identity;
    octet_t port_state;
    octet_t timestamping;
    octet_t interface_length;
};
static_assert(sizeof(PortPropertiesNpPayload) == PortPropertiesNpPayload::LENGTH);
static_assert(offsetof(PortPropertiesNpPayload, interface_length) == 12);

/// linuxptp's PORT_DATA_SET_NP (struct port_ds_np): neighborPropDelayThresh in ns, then asCapable.
struct PortDataSetNpPayload
{
    static constexpr size_t LENGTH = 8;
    quadlet_t neighbor_prop_delay_thresh;
    quadlet_t as_capable;
};
static_assert(sizeof(PortDataSetNpPayload) == PortDataSetNpPayload::LENGTH);
static_assert(offsetof(PortDataSetNpPayload, as_capable) == 4);

/// TIME_STATUS_NP: lastGmPhaseChange is a ScaledNs (16-bit msb, 64-bit lsb, 16-bit fraction).
struct TimeStatusNpPayload
{
    static constexpr size_t LENGTH = 50;
    octlet_t master_offset;
    octlet_t ingress_time;
    quadlet_t cumulative_scaled_rate_offset;
    quadlet_t scaled_last_gm_phase_change;
    doublet_t gm_time_base_indicator;
    std::array<uint8_t, 12> last_gm_phase_change;
    quadlet_t gm_present;
    Eui64 gm_identity;
};
static_assert(sizeof(TimeStatusNpPayload) == TimeStatusNpPayload::LENGTH);
static_assert(offsetof(TimeStatusNpPayload, gm_time_base_indicator) == 24);
static_assert(offsetof(TimeStatusNpPayload, gm_present) == 38);
static_assert(offsetof(TimeStatusNpPayload, gm_identity) == 42);

/// GRANDMASTER_SETTINGS_NP: clockQuality (Clause 7.6.2.5), currentUtcOffset, timeFlags, timeSource.
struct GrandmasterSettingsNpPayload
{
    static constexpr size_t LENGTH = 8;
    octet_t clock_class;
    octet_t clock_accuracy;
    doublet_t offset_scaled_log_variance;
    doublet_t current_utc_offset;
    octet_t time_flags;
    octet_t time_source;
};
static_assert(sizeof(GrandmasterSettingsNpPayload) == GrandmasterSettingsNpPayload::LENGTH);
static_assert(offsetof(GrandmasterSettingsNpPayload, current_utc_offset) == 4);
static_assert(offsetof(GrandmasterSettingsNpPayload, time_source) == 7);

/// SUBSCRIBE_EVENTS_NP: a duration in seconds (65535: never expires) and the event bitmask.
struct SubscribeEventsNpPayload
{
    static constexpr size_t LENGTH = 66;
    static constexpr size_t BITMASK_LENGTH = 64;  ///< EVENT_BITMASK_CNT
    doublet_t duration;
    std::array<uint8_t, BITMASK_LENGTH> bitmask;
};
static_assert(sizeof(SubscribeEventsNpPayload) == SubscribeEventsNpPayload::LENGTH);
static_assert(offsetof(SubscribeEventsNpPayload, bitmask) == 2);

}  // namespace statusbar::ptpclient
