#pragma once
// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file atdecc_aem_children.hpp
/// @brief Which descriptors an AEM descriptor owns, and a walk that follows them.
///
/// IEEE 1722.1-2021 7.2.2 has a CONFIGURATION's descriptor_counts list the top
/// level descriptors only. Every other descriptor is reached from the one
/// that owns it, which names a first index and a count: a unit names its
/// stream ports, a stream port its clusters and maps, a LOCALE its STRINGS, a
/// PTP_INSTANCE its PTP_PORTs. A controller that reads only what the counts
/// list never sees those, so a walk of an entity's model follows the counts
/// and then each descriptor it reads to the descriptors beneath it.
///
/// Types the counts may list are followed from their owners too (a unit's
/// CONTROLs, say): entities differ on whether the count of such a type is
/// the configuration's own or the total, and a reader that dedupes loses
/// nothing by being told twice.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>

namespace statusbar::atdecc::aem {

/// A run of consecutively indexed descriptors of one type.
struct DescriptorRange
{
    uint16_t type{0};
    uint16_t base{0};
    uint16_t count{0};

    auto operator<=>(DescriptorRange const&) const noexcept = default;
};

/// The most runs one descriptor names: the sixteen of a unit.
inline constexpr size_t MAX_CHILD_RANGES = 16;
using DescriptorRanges = statusbar::sg14::inplace_vector<DescriptorRange, MAX_CHILD_RANGES>;

/// Whether @p type is AUDIO_UNIT, VIDEO_UNIT or SENSOR_UNIT.
[[nodiscard]] constexpr auto is_unit_descriptor(uint16_t const type) noexcept -> bool
{
    return type == DESCRIPTOR_AUDIO_UNIT || type == DESCRIPTOR_VIDEO_UNIT || type == DESCRIPTOR_SENSOR_UNIT;
}

/// Whether @p type is STREAM_PORT_INPUT or STREAM_PORT_OUTPUT.
[[nodiscard]] constexpr auto is_stream_port_descriptor(uint16_t const type) noexcept -> bool
{
    return type == DESCRIPTOR_STREAM_PORT_INPUT || type == DESCRIPTOR_STREAM_PORT_OUTPUT;
}

/// The runs of descriptors that @p descriptor owns, the empty ones left out.
/// @p descriptor is a descriptor as READ_DESCRIPTOR returns it, its type and
/// index first; one cut short (an older revision's length, a truncated read)
/// gives the runs its octets hold. A descriptor that owns none gives none.
///
/// A stream port's clusters and maps come back as AUDIO_CLUSTER and
/// AUDIO_MAP: the port does not say what its unit carries. range_under_unit()
/// turns them into the unit's own.
[[nodiscard]] auto child_descriptor_ranges(std::span<uint8_t const> descriptor) -> DescriptorRanges;

/// @p range as it is beneath a unit of @p unit_type: a stream port's cluster
/// and map runs take the video or sensor type under a VIDEO_UNIT or a
/// SENSOR_UNIT (IEEE 1722.1-2021 Table 7-23). Any other run is unchanged.
[[nodiscard]] auto range_under_unit(DescriptorRange range, uint16_t unit_type) noexcept -> DescriptorRange;

/// Follows one entity's descriptors from owner to owned.
///
/// Given each descriptor as it is read, it names the descriptors beneath it.
/// It keeps which unit owns each stream port, so a port's clusters and maps
/// are named with the right type whichever of the two was read first: a port
/// read before its unit waits for it.
///
/// The model is the entity's word, so the walk is bounded: it names no index
/// at or above index_limit, and stops for good after max_steps indexes
/// (exhausted() then says so). A step is one index looked at, whether the
/// reader had it already or not.
class DescriptorChildWalk
{
  public:
    /// Told of each descriptor found: its type and its index.
    using Visit = statusbar::sg14::inplace_function<void(uint16_t type, uint16_t index), 32>;

    /// Four times the largest model met (a processor of 55,000 CONTROLs, each
    /// beneath one owner).
    static constexpr size_t DEFAULT_MAX_STEPS = 400'000;
    static constexpr uint16_t DEFAULT_INDEX_LIMIT = 60000;

    DescriptorChildWalk() noexcept = default;

    DescriptorChildWalk(size_t const max_steps, uint16_t const index_limit) noexcept
        : steps_left_{max_steps}
        , index_limit_{index_limit}
    {}

    /// @p descriptor of the entity was read: tell @p visit of every descriptor
    /// it owns. A stream port whose unit is not known yet is held, and its
    /// descriptors are told when the unit is read.
    void descriptor(std::span<uint8_t const> descriptor, Visit const& visit);

    /// Whether the walk ran out of steps and left descriptors unnamed.
    [[nodiscard]] auto exhausted() const noexcept -> bool { return exhausted_; }

  private:
    /// A descriptor of the entity: its type and its index.
    using Key = std::pair<uint16_t, uint16_t>;

    void unit(uint16_t unit_type, DescriptorRanges const& ranges, Visit const& visit);
    void stream_port(Key const& port, DescriptorRanges const& ranges, Visit const& visit);
    void adopt_ports(DescriptorRange const& ports, uint16_t unit_type, Visit const& visit);
    void tell_waiting_port(Key const& port, uint16_t unit_type, Visit const& visit);
    void tell_ranges(DescriptorRanges const& ranges, uint16_t unit_type, Visit const& visit);
    void tell_range(DescriptorRange const& range, Visit const& visit);
    [[nodiscard]] auto take_step() noexcept -> bool;

    /// The indexes of @p range that the bounds allow, in order.
    template <class Fn>
    void each_index(DescriptorRange const& range, Fn&& fn)
    {
        for (uint32_t offset = 0; offset < range.count; ++offset) {
            uint32_t const index = uint32_t{range.base} + offset;
            if (index >= index_limit_ || !take_step()) {
                return;
            }
            fn(static_cast<uint16_t>(index));
        }
    }

    std::map<Key, uint16_t> port_units_;
    std::map<Key, DescriptorRanges> waiting_ports_;
    size_t steps_left_{DEFAULT_MAX_STEPS};
    uint16_t index_limit_{DEFAULT_INDEX_LIMIT};
    bool exhausted_{false};
};

}  // namespace statusbar::atdecc::aem
