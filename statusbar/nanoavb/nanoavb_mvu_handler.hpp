#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB MVU handler - AECP Milan Vendor Unique commands (Milan v1.2 Sections 5.4.3-5.4.4),
/// beside the AEM handler (nanoavb_entity.hpp). What is implemented and what is answered is
/// in docs/MVU.md.

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_profile.hpp"

#include <cstdint>
#include <span>

namespace statusbar::nanoavb {

/// MvuCommandHandler - answers MVU commands addressed to this entity: no packet I/O, the caller
/// hands it frames and a `send_response` callback. Single-threaded: the reactor thread that
/// drives the AEM handler.
class MvuCommandHandler
{
  public:
    /// The entity's profile (EntityProfile): MVU commands are answered only in
    /// the Milan profile. NanoAvbComponents sets it at construction. Default:
    /// Milan.
    void set_profile(EntityProfile const profile) noexcept { profile_ = profile; }
    [[nodiscard]] auto profile() const noexcept -> EntityProfile { return profile_; }

    /// Set the callback that sends a response, unicast to the controller
    void set_send_response(SendAemResponseFn fn) { send_response_ = std::move(fn); }

    /// GET_MILAN_INFO certification_version (atdecc::mvu::milan_certification_version);
    /// 0, the default, until the entity has passed Milan certification.
    void set_certification_version(uint32_t const version) noexcept { certification_version_ = version; }
    [[nodiscard]] auto certification_version() const noexcept -> uint32_t { return certification_version_; }

    /// Process an MVU command (atdecc::mvu::is_mvu_command) from @p src_mac, @p payload starting
    /// at the AECP common header, for @p our_entity_id, and send the response.
    /// @return true if a response was sent, false if the frame was ignored
    [[nodiscard]] auto process_packet(Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id) -> bool;

  private:
    /// Send a response to @p command with @p status and @p body as its
    /// command_specific_data, padded to atdecc::mvu::MVU_MIN_PDU_LENGTH
    [[nodiscard]] auto send_response(
        Eui48 const& dest_mac, atdecc::mvu::MvuDu const& command, uint8_t status, std::span<uint8_t const> body) const -> bool;

    /// Answer @p command (control data as declared) by echoing it with status
    /// NOT_IMPLEMENTED, padded to atdecc::mvu::MVU_MIN_PDU_LENGTH
    [[nodiscard]] auto send_not_implemented(Eui48 const& dest_mac, std::span<uint8_t const> command) const -> bool;

    EntityProfile profile_{};
    uint32_t certification_version_{0};
    SendAemResponseFn send_response_{};
};

/// Route one AECPDU addressed to the entity: an MVU command to @p mvu, anything else to @p aem.
/// @return the handler's result: true if a response was sent
[[nodiscard]] auto dispatch_aecp(
    AemCommandHandler& aem,
    MvuCommandHandler& mvu,
    Eui48 const& src_mac,
    std::span<uint8_t const> payload,
    Eui64 const& our_entity_id) -> bool;

}  // namespace statusbar::nanoavb
