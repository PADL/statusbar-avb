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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace statusbar::nanoavb {

/// A media_clock_domain_name field (64 octets, zero padded) holding @p name,
/// truncated to 64 octets.
[[nodiscard]] constexpr auto mcr_domain_name(std::string_view const name) noexcept -> std::array<char, 64>
{
    std::array<char, 64> field{};
    for (size_t i = 0; i < name.size() && i < field.size(); ++i) {
        field[i] = name[i];
    }
    return field;
}

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

    /// The AEM handler whose lock and acquisition gate the SETs and whose registrations receive
    /// their notifications; without one, nothing is locked and nobody is notified.
    void set_aem_handler(AemCommandHandler* aem) noexcept { aem_ = aem; }

    /// Called (reactor thread) after a SET_SYSTEM_UNIQUE_ID or
    /// SET_MEDIA_CLOCK_REFERENCE_INFO that changed a value, once its response
    /// and notifications have gone: the entity saves them.
    using MvuSettingsChangedFn = sg14::inplace_function<void(), 32>;
    void set_on_settings_changed(MvuSettingsChangedFn fn) { on_settings_changed_ = std::move(fn); }

    // ---- SYSTEM_UNIQUE_ID ----------------------------------------------------
    [[nodiscard]] auto system_unique_id() const noexcept -> uint32_t { return system_unique_id_; }
    /// Restore a saved system unique id (no notification).
    void set_system_unique_id(uint32_t const id) noexcept { system_unique_id_ = id; }

    // ---- MEDIA_CLOCK_REFERENCE_INFO (one per CLOCK_DOMAIN) --------------------
    /// The CLOCK_DOMAIN descriptors the entity has (others are refused
    /// NO_SUCH_DESCRIPTOR); at most MAX_CLOCK_DOMAINS. Default 1.
    static constexpr size_t MAX_CLOCK_DOMAINS = 4;
    void set_clock_domain_count(size_t count) noexcept;
    [[nodiscard]] auto clock_domain_count() const noexcept -> size_t { return clock_domain_count_; }

    /// The vendor's default Media Clock Reference priority (Table 7.2) for every clock domain;
    /// a user priority still at the old default follows it. Default MVU_MCR_PRIO_NETWORK_INTERFACE.
    void set_default_mcr_priority(uint8_t prio) noexcept;
    [[nodiscard]] auto default_mcr_priority() const noexcept -> uint8_t { return default_mcr_prio_; }

    /// One clock domain's user settings (Section 7.6): the user priority and
    /// the media clock domain name (64 octets, zero padded, no NUL needed).
    struct McrInfo
    {
        uint8_t user_mcr_prio{atdecc::mvu::MVU_MCR_PRIO_NETWORK_INTERFACE};
        std::array<char, 64> domain_name{mcr_domain_name(atdecc::mvu::MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME)};

        auto operator==(McrInfo const&) const noexcept -> bool = default;
    };
    /// Clock domain @p index's settings; nullptr past clock_domain_count().
    [[nodiscard]] auto mcr_info(size_t index) const noexcept -> McrInfo const*;
    /// Restore clock domain @p index's saved settings (no notification); false
    /// past clock_domain_count().
    auto set_mcr_info(size_t index, McrInfo const& info) noexcept -> bool;

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

    /// The SET_SYSTEM_UNIQUE_ID command @p command (control data as declared)
    [[nodiscard]] auto set_system_unique_id_command(Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command)
        -> bool;
    /// The SET_MEDIA_CLOCK_REFERENCE_INFO command @p command
    [[nodiscard]] auto set_mcr_info_command(Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command)
        -> bool;
    /// The GET_MEDIA_CLOCK_REFERENCE_INFO command @p command
    [[nodiscard]] auto get_mcr_info_command(Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command)
        -> bool;
    /// Clock domain @p index's MCR payload, as a response carries it
    [[nodiscard]] auto mcr_payload(uint16_t index) const noexcept -> atdecc::mvu::MvuMediaClockReferenceInfoPayload;
    /// Refuse a SET from @p header's controller while another holds the entity
    [[nodiscard]] auto locked_against(atdecc::mvu::MvuDu const& header) const noexcept -> bool;
    /// Send @p body as an unsolicited @p command_type response (u bit set) to
    /// every registered controller but @p requester, then report the change
    void notify_changed(uint16_t command_type, Eui64 const& requester, std::span<uint8_t const> body);

    EntityProfile profile_{};
    uint32_t certification_version_{0};
    SendAemResponseFn send_response_{};
    AemCommandHandler* aem_{nullptr};
    MvuSettingsChangedFn on_settings_changed_{};
    Eui64 our_entity_id_{};
    uint32_t system_unique_id_{0};
    uint8_t default_mcr_prio_{atdecc::mvu::MVU_MCR_PRIO_NETWORK_INTERFACE};
    size_t clock_domain_count_{1};
    std::array<McrInfo, MAX_CLOCK_DOMAINS> mcr_{};
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
