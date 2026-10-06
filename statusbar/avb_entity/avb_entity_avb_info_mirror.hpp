// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

/// AvbInfoMirror — the AVB interface state of another IEEE 1722.1 entity,
/// read over 1722.1 so a proxy entity can present it as its own.
///
/// A proxy stands in for a device it controls through some other channel;
/// its AVB_INTERFACE descriptor and GET_AVB_INFO should describe the gPTP
/// port of the device it mirrors, not the proxy host's. The mirror is a
/// minimal ATDECC controller aimed at one target entity: it waits for the
/// target's ADP, reads its AVB_INTERFACE descriptors (index 0 upward until
/// NO_SUCH_DESCRIPTOR), registers for unsolicited notifications, and polls
/// GET_AVB_INFO for each interface — unsolicited GET_AVB_INFO responses
/// arrive sooner. Every descriptor and every changed AVB info reaches the
/// owner through one update callback.
///
/// The network is injected: the owner feeds received ATDECC frames in via
/// receive_frame() and supplies the send callbacks, so the whole exchange
/// is unit-testable with crafted frames. AvbInfoMirrorPort is the raw-socket
/// Pollable that does this on a real interface.
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace statusbar::avb_entity {

class AvbInfoMirror
{
  public:
    static constexpr size_t MAX_INTERFACES = 4;

    struct Config
    {
        ieee::Eui64 controller_entity_id{};  ///< the id this mirror sends commands as
        ieee::Eui64 target_entity_id{};      ///< the entity whose interfaces are mirrored
        int64_t poll_interval_ns = 5'000'000'000;
        int64_t discover_interval_ns = 2'000'000'000;  ///< ENTITY_DISCOVER cadence until the target answers
    };

    /// One mirrored interface: the descriptor once read, the AVB info once
    /// answered.
    struct Interface
    {
        uint16_t index{0};
        std::optional<atdecc::aem::DescriptorAvbInterface> descriptor{};
        std::optional<nanoavb::AvbInfo> info{};
        int64_t last_poll_ns{0};
    };

    /// Fires on every descriptor read and every CHANGED AVB info for
    /// @p index (nullptr for the half that did not change in this update).
    using UpdateFn = statusbar::sg14::inplace_function<
        void(uint16_t index, atdecc::aem::DescriptorAvbInterface const* descriptor, nanoavb::AvbInfo const* info),
        64>;
    using SendMulticastFn = statusbar::sg14::inplace_function<bool(std::span<uint8_t const> packet), 64>;
    using SendUnicastFn = statusbar::sg14::inplace_function<bool(ieee::Eui48 const& dest, std::span<uint8_t const> packet), 64>;

    AvbInfoMirror(Config config, SendMulticastFn send_multicast, SendUnicastFn send_unicast);

    void set_on_update(UpdateFn fn) { on_update_ = std::move(fn); }

    /// Feed one received ATDECC frame (the AVTP payload after the Ethernet
    /// header, as RawnetContext::recv delivers it).
    void receive_frame(ieee::Eui48 const& src_mac, std::span<uint8_t const> payload, int64_t now_ns);

    /// Drive discovery, the descriptor reads, registration and the polls.
    void tick(int64_t now_ns);

    [[nodiscard]] auto config() const noexcept -> Config const& { return config_; }
    [[nodiscard]] auto target_seen() const noexcept -> bool { return target_seen_; }
    [[nodiscard]] auto interfaces() const noexcept -> std::span<Interface const> { return {interfaces_.data(), interface_count_}; }
    [[nodiscard]] auto find(uint16_t index) const noexcept -> Interface const*;

  private:
    void wire_controller();
    void on_target_available();
    void handle_response(uint16_t cmd, uint8_t status, std::span<uint8_t const> sent, std::span<uint8_t const> data);
    void handle_read_descriptor(std::span<uint8_t const> sent, uint8_t status, std::span<uint8_t const> data);
    void handle_avb_info(std::span<uint8_t const> data);
    [[nodiscard]] auto interface_at(uint16_t index) -> Interface&;

    Config config_;
    nanoavb::NanoAvbAemController controller_;
    UpdateFn on_update_{};
    bool target_seen_{false};
    bool registered_{false};
    bool reading_{false};           ///< a READ_DESCRIPTOR is in flight
    uint16_t next_read_index_{0};   ///< the next AVB_INTERFACE index to read
    bool descriptors_done_{false};  ///< NO_SUCH_DESCRIPTOR reached
    int64_t last_discover_ns_{0};
    std::array<Interface, MAX_INTERFACES> interfaces_{};
    size_t interface_count_{0};
};

/// AvbInfoMirror on a real interface: a raw-socket Pollable that receives
/// ATDECC frames for the mirror and sends its commands.
class AvbInfoMirrorPort : public net::Pollable
{
  public:
    AvbInfoMirrorPort(std::string_view interface_name, AvbInfoMirror::Config config);

    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }
    [[nodiscard]] auto mirror() noexcept -> AvbInfoMirror& { return mirror_; }

    // -- net::Pollable --
    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override { mirror_.tick(now_ns); }
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    AvbInfoMirror mirror_;
    std::array<uint8_t, 2048> payload_buf_{};
};

}  // namespace statusbar::avb_entity
