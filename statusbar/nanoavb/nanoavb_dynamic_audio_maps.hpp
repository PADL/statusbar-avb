#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The dynamic audio mappings of an entity's stream ports (IEEE 1722.1-2021 Clauses 7.4.44-7.4.46):
/// a fixed-capacity store kept sorted by port, then cluster order, so that each port's mappings
/// are one contiguous span. An entity with dynamic ports owns one (about 20 KB, no heap) and hands
/// it to AemCommandHandler, which validates what goes in; the store only orders it.

#include "statusbar/sg14/inplace_vector.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>

namespace statusbar::nanoavb {

/// One audio mapping (the AemAudioMapping wire entry, host order): channel @p stream_channel of
/// stream @p stream_index to channel @p cluster_channel of the port's cluster at @p cluster_offset
/// (from its base_cluster).
struct DynamicAudioMapping
{
    uint16_t stream_index{0};
    uint16_t stream_channel{0};
    uint16_t cluster_offset{0};
    uint16_t cluster_channel{0};

    auto operator<=>(DynamicAudioMapping const&) const noexcept = default;
};

/// Cluster order: (cluster_offset, cluster_channel), then (stream_index, stream_channel).
[[nodiscard]] constexpr auto cluster_order(DynamicAudioMapping const& a, DynamicAudioMapping const& b) noexcept -> bool
{
    auto const key = [](DynamicAudioMapping const& m) {
        return std::tuple{m.cluster_offset, m.cluster_channel, m.stream_index, m.stream_channel};
    };
    return key(a) < key(b);
}

class DynamicAudioMaps
{
  public:
    /// Mappings kept over all ports of both directions: an input's cluster channel and an output's
    /// stream channel have at most one each, so 16 ports of 64 channels fit.
    static constexpr size_t MAX_MAPPINGS = 1024;
    /// A port's mappings as a value, for a caller that builds a new set beside the store.
    using Mappings = sg14::inplace_vector<DynamicAudioMapping, MAX_MAPPINGS>;

    /// Port (@p port_type, @p port_index)'s mappings in cluster order: a view into the store,
    /// valid until the store next changes.
    [[nodiscard]] auto port(uint16_t const port_type, uint16_t const port_index) const noexcept
        -> std::span<DynamicAudioMapping const>
    {
        auto const [first, last] = bounds(key_for(port_type, port_index));
        return std::span<DynamicAudioMapping const>{mappings_}.subspan(first, last - first);
    }

    /// Make @p mappings the port's, sorted into cluster order with duplicates dropped. False, with
    /// nothing changed, when they (duplicates counted) do not fit beside the other ports'.
    /// @p mappings must not be a view of this store.
    [[nodiscard]] auto replace(
        uint16_t const port_type, uint16_t const port_index, std::span<DynamicAudioMapping const> const mappings) noexcept -> bool
    {
        if (!fits(port_type, port_index, mappings.size())) {
            return false;
        }
        auto const key = key_for(port_type, port_index);
        auto const first = erase_port(key);
        insert_port(key, first, mappings);
        return true;
    }

    /// Whether @p count mappings fit as the port's in place of those it has.
    [[nodiscard]] auto fits(uint16_t const port_type, uint16_t const port_index, size_t const count) const noexcept -> bool
    {
        return size() - port(port_type, port_index).size() + count <= MAX_MAPPINGS;
    }

    /// @p fn(port_type, port_index, mapping) for every mapping, by port, each port's in cluster order.
    template <typename Fn>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) fn is called, not forwarded
    void for_each(Fn&& fn) const
    {
        for (size_t i = 0; i < mappings_.size(); ++i) {
            fn(static_cast<uint16_t>(keys_[i] >> 16U), static_cast<uint16_t>(keys_[i] & 0xFFFFU), mappings_[i]);
        }
    }

    [[nodiscard]] auto size() const noexcept -> size_t { return mappings_.size(); }

    /// Room for the set a change of one port builds before replace() keeps it, so whoever changes
    /// the store needs none of its own; it holds nothing between changes.
    [[nodiscard]] auto next() noexcept -> Mappings& { return next_; }

    /// Forget every port's mappings.
    void clear() noexcept
    {
        keys_.clear();
        mappings_.clear();
    }

  private:
    [[nodiscard]] static constexpr auto key_for(uint16_t const port_type, uint16_t const port_index) noexcept -> uint32_t
    {
        return (uint32_t{port_type} << 16U) | port_index;
    }

    /// The positions [first, last) of the mappings of the port @p key names.
    [[nodiscard]] auto bounds(uint32_t const key) const noexcept -> std::pair<size_t, size_t>
    {
        auto const range = std::ranges::equal_range(keys_, key);
        return {static_cast<size_t>(range.begin() - keys_.begin()), static_cast<size_t>(range.end() - keys_.begin())};
    }

    /// Erase the port's mappings; where they began, and where its new ones go.
    auto erase_port(uint32_t const key) noexcept -> size_t
    {
        auto const [first, last] = bounds(key);
        keys_.erase(keys_.begin() + first, keys_.begin() + last);
        mappings_.erase(mappings_.begin() + first, mappings_.begin() + last);
        return first;
    }

    /// Insert @p mappings at @p first as the port's, then sort them and drop duplicates.
    void insert_port(uint32_t const key, size_t const first, std::span<DynamicAudioMapping const> const mappings) noexcept
    {
        auto* const at = mappings_.insert(mappings_.begin() + first, mappings.begin(), mappings.end());
        std::ranges::sort(at, at + mappings.size(), cluster_order);
        auto const duplicates = std::ranges::unique(at, at + mappings.size());
        auto const kept = static_cast<size_t>(duplicates.begin() - at);
        mappings_.erase(duplicates.begin(), duplicates.end());
        keys_.insert(keys_.begin() + first, kept, key);
    }

    // Each mapping's port (port_type << 16 | port_index), parallel to mappings_; both sorted by it.
    sg14::inplace_vector<uint32_t, MAX_MAPPINGS> keys_{};
    sg14::inplace_vector<DynamicAudioMapping, MAX_MAPPINGS> mappings_{};
    Mappings next_{};
};

}  // namespace statusbar::nanoavb
