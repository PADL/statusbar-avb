#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ListenerVlan: keeps the listener's SR-class VID on the NIC's receive VLAN filter by holding a
/// down `<interface>.<vid>` VLAN link, or an up one with no address for IP listeners; VlanFilterTable
/// holds every VID an AVB listener receives on, refcounted. Never fatal; a failure is a warning. Linux
/// only. See docs/SHAPER.md ("Listener VLAN").

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

/// Every VID the listener receives on (the SR-class VID and each settled input's) on the NIC's receive
/// VLAN filter, refcounted: one ListenerVlan, and so one `<interface>.<vid>` link, per VID while anything
/// holds it, deleted with the last holder. A VID moves by holding the new one before dropping the old.
template <size_t Capacity>
class VlanFilterTable
{
  public:
    explicit VlanFilterTable(logging::Logger log) noexcept
        : log_{log}
    {}

    /// Hold VIDs through @p control on @p interface_name (@p ifindex) from now on, those held before
    /// released; @p control must outlive release().
    void bind(net::vlan::VlanControl& control, std::string_view const interface_name, int const ifindex)
    {
        release();
        control_ = &control;
        interface_name_ = interface_name;
        ifindex_ = ifindex;
    }
    [[nodiscard]] auto bound() const noexcept -> bool { return control_ != nullptr; }
    /// Hold @p vid once more (1..4094; others, or unbound, nothing); its link is created at the first hold,
    /// and tried again at each later one while it is missing (a refused create is warned).
    void hold(uint16_t const vid)
    {
        if (control_ == nullptr || vid == 0 || vid > ListenerVlan::MAX_VID) {
            return;
        }
        if (auto* const held = find(vid); held != nullptr) {
            ++held->count;
            follow(*held);
            return;
        }
        add(vid);
    }
    /// Drop one hold of @p vid; the last deletes its link (if created here).
    void drop(uint16_t const vid)
    {
        auto* const held = find(vid);
        if (held != nullptr && --held->count == 0) {
            clear(*held);
        }
    }
    /// Drop every VID and unbind.
    void release()
    {
        for (auto& entry : entries_) {
            clear(entry);
        }
        control_ = nullptr;
    }
    /// How many holds @p vid has.
    [[nodiscard]] auto holds(uint16_t const vid) const noexcept -> uint16_t
    {
        auto const* const held = std::ranges::find_if(entries_, [vid](Entry const& e) { return e.count != 0 && e.vid == vid; });
        return held != entries_.end() ? held->count : uint16_t{0};
    }

  private:
    struct Entry
    {
        uint16_t vid{0};
        uint16_t count{0};  ///< 0: free
        std::optional<ListenerVlan> link{};
    };

    /// Free @p entry, its link deleted if created here.
    static void clear(Entry& entry) noexcept
    {
        entry.link.reset();
        entry.vid = 0;
        entry.count = 0;
    }

    [[nodiscard]] auto find(uint16_t const vid) noexcept -> Entry*
    {
        auto* const held = std::ranges::find_if(entries_, [vid](Entry const& e) { return e.count != 0 && e.vid == vid; });
        return held != entries_.end() ? &*held : nullptr;
    }
    /// A first hold of @p vid in a free entry; warned when Capacity are held.
    void add(uint16_t const vid)
    {
        auto* const free = std::ranges::find(entries_, uint16_t{0}, &Entry::count);
        if (free == entries_.end()) {
            log_.warning("listener: VID {} not put on the NIC's VLAN filter: {} VIDs held already", vid, Capacity);
            return;
        }
        open(*free, vid);
    }
    /// @p entry holding @p vid once, its link created.
    void open(Entry& entry, uint16_t const vid)
    {
        entry.vid = vid;
        entry.count = 1;
        entry.link.emplace(log_).bind(*control_, interface_name_, ifindex_);
        follow(entry);
    }
    /// Create @p entry's link unless it is there.
    static void follow(Entry& entry)
    {
        if (entry.link && entry.link->vid() != entry.vid) {
            entry.link->follow(entry.vid);
        }
    }

    std::array<Entry, Capacity> entries_{};
    net::vlan::VlanControl* control_{nullptr};
    std::string interface_name_{};
    int ifindex_{0};
    logging::Logger log_;
};

}  // namespace statusbar::avb_entity

#endif  // __linux__
