#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ListenerVlan: keeps the listener's SR-class VID on the NIC's receive VLAN filter by holding a
/// down `<interface>.<vid>` VLAN link, or an up one with no address for IP listeners; SinkVlanFilters
/// holds the other VIDs listener streams use. Never fatal; a failure is a warning. Linux only. See
/// docs/SHAPER.md ("Listener VLAN").

#if defined(__linux__)

#    include "statusbar/logging/logging.hpp"
#    include "statusbar/net/net_vlan_link.hpp"

#    include <algorithm>
#    include <array>
#    include <cstddef>
#    include <cstdint>
#    include <optional>
#    include <string>
#    include <string_view>

namespace statusbar::avb_entity {

/// What the VLAN link is for.
enum class ListenerLinkUse : uint8_t
{
    vlan_filter,   ///< the NIC's receive VLAN filter alone: the link stays down
    ip_multicast,  ///< also IP group memberships on it, reported tagged (IGMP snooping): up, no address; one there is adopted
};

class ListenerVlan
{
  public:
    explicit ListenerVlan(logging::Logger log, ListenerLinkUse use = ListenerLinkUse::vlan_filter) noexcept
        : log_{log}
        , use_{use}
    {}
    ListenerVlan(ListenerVlan const&) = delete;
    auto operator=(ListenerVlan const&) -> ListenerVlan& = delete;
    ListenerVlan(ListenerVlan&&) = delete;
    auto operator=(ListenerVlan&&) -> ListenerVlan& = delete;
    /// release(): @p control must outlive this.
    ~ListenerVlan() { release(); }

    /// Register through @p control on the AVB interface @p interface_name
    /// (@p ifindex) from now on. follow() is a no-op until bound.
    void bind(net::vlan::VlanControl& control, std::string_view interface_name, int ifindex);
    /// Make @p vid the registered VID: create its link (unless present), then delete the previous
    /// one if this created it. No-op when unbound or unchanged; a VID outside 1..4094 or a failed
    /// create changes nothing.
    void follow(uint16_t vid);
    /// Delete the link if this created it, forget the VID and unbind (a
    /// later follow() is a no-op until bind()).
    void release();

    [[nodiscard]] auto bound() const noexcept -> bool { return control_ != nullptr; }
    /// The VID followed (0: none).
    [[nodiscard]] auto vid() const noexcept -> uint16_t { return vid_; }
    /// Whether this created (or, for IP listeners, adopted) the link for vid(), and will delete it.
    [[nodiscard]] auto created() const noexcept -> bool { return created_; }
    /// Whether this brought the link for vid() up (ListenerLinkUse::ip_multicast).
    [[nodiscard]] auto up() const noexcept -> bool { return up_; }
    /// The link for vid() (empty: none followed).
    [[nodiscard]] auto link_name() const noexcept -> std::string const& { return name_; }

    static constexpr uint16_t MAX_VID = 4094;

  private:
    /// What add_link did with a VID's link.
    enum class LinkAdded : uint8_t
    {
        created,  ///< created here
        adopted,  ///< already there, adopted as created here (ListenerLinkUse::ip_multicast)
        existed,  ///< already there, used as it is
        failed,   ///< not created (logged)
    };
    /// Create @p vid's link @p name.
    [[nodiscard]] auto add_link(std::string const& name, uint16_t vid) -> LinkAdded;
    /// What becomes of @p vid's link @p name, already there.
    [[nodiscard]] auto existing_link(std::string const& name, uint16_t vid) -> LinkAdded;
    /// Delete the previous link if this created it, then follow @p vid through @p name (brought up if @p created).
    void replace_link(uint16_t vid, bool created, std::string name);
    /// Bring the created link @p name up for ListenerLinkUse::ip_multicast: whether it is up.
    [[nodiscard]] auto bring_up(std::string const& name) -> bool;

    net::vlan::VlanControl* control_{nullptr};
    std::string interface_name_{};
    int ifindex_{0};
    logging::Logger log_;
    ListenerLinkUse use_;
    uint16_t vid_{0};
    bool created_{false};
    bool up_{false};
    std::string name_{};
};

/// The listener streams' VIDs besides the SR-class one (ListenerVlan's) on the NIC's receive VLAN
/// filter: a Milan sink settles on the VID its talker's probe answer gives (Milan v1.3 Section 5.5.3.6).
/// One ListenerVlan per VID held, at most @p Capacity.
template <size_t Capacity>
class SinkVlanFilters
{
  public:
    explicit SinkVlanFilters(logging::Logger log) noexcept
        : log_{log}
    {}

    /// Hold VIDs through @p control on @p interface_name (@p ifindex) from now on, those held before released;
    /// @p control must outlive release().
    void bind(net::vlan::VlanControl& control, std::string_view const interface_name, int const ifindex)
    {
        release();
        control_ = &control;
        interface_name_ = interface_name;
        ifindex_ = ifindex;
    }
    /// Put @p vid on the filter; a no-op when held or unbound, warned when Capacity are held.
    void hold(uint16_t const vid)
    {
        if (control_ == nullptr || slot_of(vid) != nullptr) {
            return;
        }
        auto* const free = slot_of(0);
        if (free == nullptr) {
            log_.warning("listener: VID {} not put on the NIC's VLAN filter: {} VIDs held already", vid, Capacity);
            return;
        }
        free->emplace(log_);
        (*free)->bind(*control_, interface_name_, ifindex_);
        (*free)->follow(vid);
        if ((*free)->vid() != vid) {
            free->reset();  // not created (warned): the slot stays free, the next hold tries again
        }
    }
    /// Remove @p vid from the filter (its link deleted if created here).
    void drop(uint16_t const vid)
    {
        if (auto* const held = slot_of(vid); held != nullptr) {
            held->reset();
        }
    }
    /// Hold each of @p vids but @p class_a, which ListenerVlan holds.
    template <class Vids>
    void hold_all(Vids const& vids, uint16_t const class_a)
    {
        for (uint16_t const vid : vids) {
            if (vid != class_a) {
                hold(vid);
            }
        }
    }
    /// Drop each VID held that is @p class_a or not one of @p vids: before ListenerVlan follows a new
    /// @p class_a, so that it creates (and owns) its link.
    template <class Vids>
    void drop_except(Vids const& vids, uint16_t const class_a)
    {
        for (auto& link : links_) {
            if (link && (link->vid() == class_a || !std::ranges::contains(vids, link->vid()))) {
                link.reset();
            }
        }
    }
    /// Remove every VID from the filter and unbind.
    void release()
    {
        for (auto& link : links_) {
            link.reset();
        }
        control_ = nullptr;
    }
    /// Whether @p vid is held.
    [[nodiscard]] auto holds(uint16_t const vid) const noexcept -> bool
    {
        return std::ranges::any_of(links_, [vid](auto const& link) { return link && link->vid() == vid; });
    }

  private:
    /// The slot holding @p vid; with 0, a free one; nullptr when none.
    [[nodiscard]] auto slot_of(uint16_t const vid) noexcept -> std::optional<ListenerVlan>*
    {
        auto* const found = std::ranges::find_if(links_, [vid](auto const& link) { return link ? link->vid() == vid : vid == 0; });
        return found != links_.end() ? &*found : nullptr;
    }

    std::array<std::optional<ListenerVlan>, Capacity> links_{};
    net::vlan::VlanControl* control_{nullptr};
    std::string interface_name_{};
    int ifindex_{0};
    logging::Logger log_;
};

}  // namespace statusbar::avb_entity

#endif  // __linux__
