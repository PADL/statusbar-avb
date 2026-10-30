// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_listener_vlan.hpp"

#if defined(__linux__)

#    include <system_error>
#    include <utility>

namespace statusbar::avb_entity {

void ListenerVlan::bind(net::vlan::VlanControl& control, std::string_view const interface_name, int const ifindex)
{
    release();  // a link this created goes with the control that created it
    control_ = &control;
    interface_name_ = interface_name;
    ifindex_ = ifindex;
}

void ListenerVlan::follow(uint16_t const vid)
{
    if (control_ == nullptr || vid == vid_) {
        return;
    }
    if (vid == 0 || vid > MAX_VID) {
        log_.warning("listener: VID {} cannot be put on the NIC's VLAN filter; keeping VID {}", vid, vid_);
        return;
    }
    std::string name = net::vlan::vlan_link_name(interface_name_, vid);
    auto const added = add_link(name, vid);
    if (added == LinkAdded::failed) {
        return;
    }
    replace_link(vid, added != LinkAdded::existed, std::move(name));
}

auto ListenerVlan::add_link(std::string const& name, uint16_t const vid) -> LinkAdded
{
    auto const status = control_->add_vlan(ifindex_, name, vid);
    if (status) {
        log_.status(
            "listener: VID {} on the NIC's VLAN filter: created {} ({})",
            vid,
            logging::embed<16>(name),
            use_ == ListenerLinkUse::ip_multicast ? logging::lit("up, no address") : logging::lit("down"));
        return LinkAdded::created;
    }
    if (status.error() == std::errc::file_exists) {
        return existing_link(name, vid);
    }
    // The previous VID's link stays; the next follow() tries again.
    log_.warning(
        "listener: VID {} not registered with the NIC's VLAN filter ({}: errno {} {}); a filtering NIC drops its streams",
        vid,
        logging::embed<16>(name),
        status.error().value(),
        logging::embed<32>(control_->last_message()));
    return LinkAdded::failed;
}

auto ListenerVlan::existing_link(std::string const& name, uint16_t const vid) -> LinkAdded
{
    // Not checked: a link of this name on another VID or parent leaves the VID unregistered.
    if (use_ == ListenerLinkUse::ip_multicast) {
        log_.warning("listener: VID {}: {} already exists (a crashed run's?); adopted as ours", vid, logging::embed<16>(name));
        return LinkAdded::adopted;
    }
    log_.warning(
        "listener: VID {}: {} (or a VLAN link on that VID) already exists; used as it is, unchecked",
        vid,
        logging::embed<16>(name));
    return LinkAdded::existed;
}

auto ListenerVlan::bring_up(std::string const& name) -> bool
{
    if (use_ != ListenerLinkUse::ip_multicast) {
        return false;
    }
    auto const status = control_->bring_up(name);
    if (!status) {
        log_.warning(
            "listener: {} not brought up (errno {} {}): no IP group membership is reported on its VID",
            logging::embed<16>(name),
            status.error().value(),
            logging::embed<32>(control_->last_message()));
    }
    return status.has_value();
}

void ListenerVlan::replace_link(uint16_t const vid, bool const created, std::string name)
{
    if (created_) {
        if (auto const status = control_->remove_link(name_); !status) {
            log_.warning("listener: deleting {} failed: errno {}", logging::embed<16>(name_), status.error().value());
        }
    }
    vid_ = vid;
    created_ = created;
    name_ = std::move(name);
    up_ = created && bring_up(name_);
}

void ListenerVlan::release()
{
    if (control_ != nullptr && created_) {
        if (auto const status = control_->remove_link(name_); !status) {
            log_.warning("listener: deleting {} failed: errno {}", logging::embed<16>(name_), status.error().value());
        } else {
            log_.status("listener: deleted {}", logging::embed<16>(name_));
        }
    }
    control_ = nullptr;
    vid_ = 0;
    created_ = false;
    up_ = false;
    name_.clear();
}

}  // namespace statusbar::avb_entity

#endif  // __linux__
