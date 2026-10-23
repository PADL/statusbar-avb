#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ListenerVlan: keeps the listener's SR-class VID on the NIC's receive VLAN filter by holding a
/// down `<interface>.<vid>` VLAN link. Never fatal; a failure is a warning. Linux only.
/// See docs/SHAPER.md ("Listener VLAN").

#if defined(__linux__)

#    include "statusbar/logging/logging.hpp"
#    include "statusbar/net/net_vlan_link.hpp"

#    include <cstdint>
#    include <string>
#    include <string_view>

namespace statusbar::avb_entity {

class ListenerVlan
{
  public:
    explicit ListenerVlan(logging::Logger log) noexcept
        : log_{log}
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
    /// Whether this created the link for vid() (and will delete it).
    [[nodiscard]] auto created() const noexcept -> bool { return created_; }
    /// The link for vid() (empty: none followed).
    [[nodiscard]] auto link_name() const noexcept -> std::string const& { return name_; }

    static constexpr uint16_t MAX_VID = 4094;

  private:
    net::vlan::VlanControl* control_{nullptr};
    std::string interface_name_{};
    int ifindex_{0};
    logging::Logger log_;
    uint16_t vid_{0};
    bool created_{false};
    std::string name_{};
};

}  // namespace statusbar::avb_entity

#endif  // __linux__
