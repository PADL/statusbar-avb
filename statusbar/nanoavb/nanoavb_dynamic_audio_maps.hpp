#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The dynamic audio mappings of an entity's stream ports (IEEE 1722.1-2021 Clauses 7.4.44-46,
/// Milan v1.2 Sections 5.3.9-5.3.10): a plain store AemCommandHandler validates against,
/// each port's mappings ordered by cluster (offset, channel), then stream (index, channel).

#include "statusbar/sg14/inplace_vector.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::nanoavb {

/// One audio mapping (the AemAudioMapping wire entry, host order): stream
/// @p stream_index's channel @p stream_channel to channel @p cluster_channel
/// of the port's cluster at @p cluster_offset (from its base_cluster).
struct DynamicAudioMapping
{
    uint16_t stream_index{0};
    uint16_t stream_channel{0};
    uint16_t cluster_offset{0};
    uint16_t cluster_channel{0};

    auto operator<=>(DynamicAudioMapping const&) const noexcept = default;
};

/// Cluster order: (cluster_offset, cluster_channel), then the stream channel.
[[nodiscard]] constexpr auto cluster_order(DynamicAudioMapping const& a, DynamicAudioMapping const& b) noexcept -> bool
{
    if (a.cluster_offset != b.cluster_offset) {
        return a.cluster_offset < b.cluster_offset;
    }
    if (a.cluster_channel != b.cluster_channel) {
        return a.cluster_channel < b.cluster_channel;
    }
    if (a.stream_index != b.stream_index) {
        return a.stream_index < b.stream_index;
    }
    return a.stream_channel < b.stream_channel;
}

class DynamicAudioMaps
{
  public:
    /// Mappings kept over all ports: an input cluster channel takes at most one (Milan v1.2
    /// Section 5.3.10.1), so 16 ports of 64 channels (Milan Formats 2.1 HC24's widest) fit.
    static constexpr size_t MAX_MAPPINGS = 1024;
    using Mappings = sg14::inplace_vector<DynamicAudioMapping, MAX_MAPPINGS>;

    /// Port (@p port_type, @p port_index)'s mappings, in cluster order.
    [[nodiscard]] auto port(uint16_t const port_type, uint16_t const port_index) const -> Mappings
    {
        Mappings out{};
        for (auto const& entry : entries_) {
            if (entry.port_type == port_type && entry.port_index == port_index) {
                out.push_back(entry.mapping);
            }
        }
        return out;
    }

    /// Replace the port's mappings with @p mappings (sorted into cluster
    /// order, duplicates dropped). False, with nothing changed, when the
    /// store would overflow.
    [[nodiscard]] auto replace(
        uint16_t const port_type, uint16_t const port_index, std::span<DynamicAudioMapping const> const mappings) -> bool
    {
        Mappings sorted{};
        for (auto const& mapping : mappings) {
            if (std::ranges::find(sorted, mapping) == sorted.end()) {
                if (sorted.size() == sorted.capacity()) {
                    return false;
                }
                sorted.push_back(mapping);
            }
        }
        std::ranges::sort(sorted, cluster_order);
        size_t const others = static_cast<size_t>(std::ranges::count_if(
            entries_, [&](Entry const& e) { return e.port_type != port_type || e.port_index != port_index; }));
        if (others + sorted.size() > MAX_MAPPINGS) {
            return false;
        }
        entries_.erase(
            std::remove_if(
                entries_.begin(),
                entries_.end(),
                [&](Entry const& e) { return e.port_type == port_type && e.port_index == port_index; }),
            entries_.end());
        for (auto const& mapping : sorted) {
            entries_.push_back({.port_type = port_type, .port_index = port_index, .mapping = mapping});
        }
        return true;
    }

    /// @p fn(port_type, port_index, mapping) for every mapping, each port's in
    /// cluster order.
    template <typename Fn>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) - fn is called, not forwarded
    void for_each(Fn&& fn) const
    {
        for (auto const& entry : entries_) {
            fn(entry.port_type, entry.port_index, entry.mapping);
        }
    }

    [[nodiscard]] auto size() const noexcept -> size_t { return entries_.size(); }
    /// Whether the store holds @p count mappings (distinct) for the port in place of its own.
    [[nodiscard]] auto fits(uint16_t const port_type, uint16_t const port_index, size_t const count) const noexcept -> bool
    {
        size_t const others = static_cast<size_t>(std::ranges::count_if(
            entries_, [&](Entry const& e) { return e.port_type != port_type || e.port_index != port_index; }));
        return others + count <= MAX_MAPPINGS;
    }

  private:
    struct Entry
    {
        uint16_t port_type{0};
        uint16_t port_index{0};
        DynamicAudioMapping mapping{};
    };
    sg14::inplace_vector<Entry, MAX_MAPPINGS> entries_{};
};

}  // namespace statusbar::nanoavb
