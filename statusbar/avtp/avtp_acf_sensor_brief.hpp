#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_SENSOR_BRIEF - Abbreviated Sensor or Actuator (SENSOR_BRIEF) message - IEEE 1722-2025 Clause 9.4.11
/// The Sensor or Actuator message (avtp_acf_sensor.hpp) without the two-quadlet
/// message_timestamp. mtv is transmitted as zero and ignored on receipt
/// (9.4.11.1).
///
/// Wire format (4-byte fixed part + payload quadlets), Figure 80:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Byte 2:      mtv[7] | num_sensors[6:0]
///   Byte 3:      sz[7:6] | sensor_group[5:0]
///   Bytes 4+:   payload, zero-padded to the quadlet (pad octets)

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

using ieee::octet_t;

/// Abbreviated Sensor or Actuator (SENSOR_BRIEF) message fixed part - IEEE 1722-2025 Clause 9.4.11, Figure 80
struct AcfSensorBriefMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 4;
    /// Payload bounds in quadlets (9.4.11)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 1;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 128;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Byte 2: mtv[7] | num_sensors[6:0]
    octet_t mtv_num;

    // Byte 3: sz[7:6] | sensor_group[5:0]
    octet_t sz_group;

    // Accessors

    /// Get mtv: message_timestamp valid - always zero on a brief message (9.4.11.1)
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return mtv_num.has_flag(0x80U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { mtv_num.set_flag(0x80U, value); }

    /// Get num_sensors: number of values (0 means 128)
    [[nodiscard]] constexpr auto num_sensors() const noexcept -> uint8_t { return mtv_num.get_bits<uint8_t>(0x7FU, 0); }

    /// Set num_sensors
    constexpr void set_num_sensors(uint8_t const value) noexcept { mtv_num.set_bits(0x7FU, 0, value); }

    /// Get sz: octets per value (0 means 4)
    [[nodiscard]] constexpr auto sz() const noexcept -> uint8_t { return sz_group.get_bits<uint8_t>(0xC0U, 6); }

    /// Set sz
    constexpr void set_sz(uint8_t const value) noexcept { sz_group.set_bits(0xC0U, 6, value); }

    /// Get sensor_group: application-specific group of sensors or actuators
    [[nodiscard]] constexpr auto sensor_group() const noexcept -> uint8_t { return sz_group.get_bits<uint8_t>(0x3FU, 0); }

    /// Set sensor_group
    constexpr void set_sensor_group(uint8_t const value) noexcept { sz_group.set_bits(0x3FU, 0, value); }

    /// Sensor or actuator values carried: num_sensors, or 128 when it is zero (9.4.10.3)
    [[nodiscard]] constexpr auto sensor_count() const noexcept -> size_t
    {
        return num_sensors() == 0U ? size_t{128} : size_t{num_sensors()};
    }

    /// Octets per value: sz, or 4 when it is zero (9.4.10.4)
    [[nodiscard]] constexpr auto sensor_value_size() const noexcept -> size_t { return sz() == 0U ? size_t{4} : size_t{sz()}; }

    /// Octets of values before quadlet padding
    [[nodiscard]] constexpr auto payload_octets() const noexcept -> size_t { return sensor_count() * sensor_value_size(); }

    /// Implied pad: this message carries no pad field; the padding is whatever the
    /// declared length holds beyond payload_octets() (zero when the length is short,
    /// which is_valid() rejects)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t
    {
        auto const octets = header.msg_length_octets();
        if (octets < LENGTH + payload_octets()) {
            return 0U;
        }
        return static_cast<uint8_t>(octets - LENGTH - payload_octets());
    }

    /// No pad field to set: the builder's payload must match the fields (is_valid())
    constexpr void set_pad(uint8_t const /*value*/) noexcept {}

    // Initialization

    /// Initialize as the smallest ACF_SENSOR_BRIEF message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::sensor_brief, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        mtv_num = 0U;
        sz_group = 0U;
    }

    // Validation

    /// Typed ACF_SENSOR_BRIEF with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::sensor_brief) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        if (octets < LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) ||
            octets > LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS)) {
            return false;
        }
        // No pad field: the declared length must be exactly the quadlets the
        // fields' payload needs (9.4.1.7)
        if (octets != LENGTH + (acf_quadlets_for_octets(payload_octets()) * ACF_QUADLET_OCTETS)) {
            return false;
        }
        return true;
    }

    auto operator<=>(AcfSensorBriefMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfSensorBriefMessage) == AcfSensorBriefMessage::LENGTH, "AcfSensorBriefMessage must be exactly 4 bytes");
static_assert(alignof(AcfSensorBriefMessage) <= 4, "AcfSensorBriefMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfSensorBriefMessage, mtv_num) == 2, "mtv_num must be at offset 2");
static_assert(offsetof(AcfSensorBriefMessage, sz_group) == 3, "sz_group must be at offset 3");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfSensorBriefMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_SENSOR_BRIEF message: fixed part plus payload (pad removed)
using AcfSensorBriefMessageView = AcfTypedMessageView<AcfSensorBriefMessage>;

/// Parse an ACF_SENSOR_BRIEF message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_sensor_brief_parse(std::span<uint8_t const> const data) noexcept
    -> std::optional<AcfSensorBriefMessageView>
{
    return acf_parse_typed<AcfSensorBriefMessage>(data);
}

/// Build an ACF_SENSOR_BRIEF message into @p out from @p fixed and the original @p payload;
/// mtv is forced to zero (brief messages carry no timestamp). Returns the octets
/// written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_sensor_brief_build(
    std::span<uint8_t> const out, AcfSensorBriefMessage fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    fixed.set_mtv(false);
    return acf_build_typed<AcfSensorBriefMessage>(out, fixed, payload);
}

/// The @p index-th sensor or actuator value of @p view as an unsigned big-endian
/// integer of sensor_value_size() octets (1 to 4); nullopt past sensor_count() or
/// the carried payload
[[nodiscard]] inline auto acf_sensor_brief_value(AcfSensorBriefMessageView const& view, size_t const index) noexcept
    -> std::optional<uint32_t>
{
    auto const size = view.fixed.sensor_value_size();
    if (index >= view.fixed.sensor_count() || ((index + 1U) * size) > view.payload.size()) {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (auto const octet : view.payload.subspan(index * size, size)) {
        value = (value << 8U) | octet;
    }
    return value;
}

}  // namespace statusbar::avtp
