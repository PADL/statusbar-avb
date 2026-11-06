#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB MVU handler - AECP Milan Vendor Unique commands (Milan v1.3 Sections 5.4.3-5.4.4,
/// or v1.2's), beside the AEM handler (nanoavb_entity.hpp). What is implemented and what is
/// answered is in docs/MVU.md.

#include "statusbar/atdecc/atdecc_acmp_milan_listener.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    /// The entity's profile (EntityProfile): MVU commands are answered only in the Milan profile,
    /// in its Milan version's layouts. NanoAvbComponents sets it. Default: Milan v1.3.
    void set_profile(EntityProfile const profile) noexcept { profile_ = profile; }
    [[nodiscard]] auto profile() const noexcept -> EntityProfile { return profile_; }

    /// Set the callback that sends a response, unicast to the controller
    void set_send_response(SendAemResponseFn fn) { send_response_ = std::move(fn); }

    /// GET_MILAN_INFO certification_version (atdecc::mvu::milan_certification_version);
    /// 0, the default, until the entity has passed Milan certification.
    void set_certification_version(uint32_t const version) noexcept { certification_version_ = version; }
    [[nodiscard]] auto certification_version() const noexcept -> uint32_t { return certification_version_; }

    /// GET_MILAN_INFO features_flags beyond MVU_BINDING (which a Milan listener gives), such as
    /// MVU_FEATURES_TALKER_SIGNAL_PRESENCE: set before the entity is advertised, as they may not
    /// change (Milan v1.3 Section 5.4.4.1).
    void set_extra_features(uint32_t const flags) noexcept { extra_features_ = flags; }
    /// The features_flags GET_MILAN_INFO answers with.
    [[nodiscard]] auto features_flags() const noexcept -> uint32_t;

    /// The Milan listener whose sinks BIND_STREAM, UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX
    /// address (Milan v1.3 Sections 5.4.4.6-8); without one, every STREAM_INPUT is unknown.
    void set_milan_listener(atdecc::MilanListener* listener) noexcept { listener_ = listener; }
    /// The STREAM_INPUT descriptors the model has (index past it: NO_SUCH_DESCRIPTOR); nullopt, the
    /// default: the listener's sinks.
    void set_stream_input_count(std::optional<size_t> const count) noexcept { stream_input_count_ = count; }

    /// Send sink @p index's GET_STREAM_INPUT_INFO_EX, unsolicited, to every registered controller
    /// (Milan v1.3 Table 5.21); nothing below Milan v1.3.
    void notify_stream_input_info_ex(uint16_t index);

    /// The AEM handler whose lock and acquisition gate the SETs and whose registrations receive
    /// their notifications; without one, nothing is locked and nobody is notified.
    void set_aem_handler(AemCommandHandler* aem) noexcept { aem_ = aem; }

    /// Called (reactor thread) after a SET_SYSTEM_UNIQUE_ID or
    /// SET_MEDIA_CLOCK_REFERENCE_INFO that changed a value, once its response
    /// and notifications have gone: the entity saves them.
    using MvuSettingsChangedFn = sg14::inplace_function<void(), 32>;
    void set_on_settings_changed(MvuSettingsChangedFn fn) { on_settings_changed_ = std::move(fn); }

    // ---- SYSTEM_UNIQUE_ID ----------------------------------------------------
    /// The system unique id's number (a u32 in Milan v1.2) and name (Milan v1.3 Section 5.4.4.2).
    [[nodiscard]] auto system_unique_id() const noexcept -> uint64_t { return system_unique_id_; }
    [[nodiscard]] auto system_unique_name() const noexcept -> std::array<char, 64> const& { return system_unique_name_; }
    /// Restore a saved system unique id (no notification): below v1.3 one past the u32, or a name, is not kept.
    void set_system_unique_id(uint64_t id, std::array<char, 64> const& name = {}) noexcept;

    // ---- MEDIA_CLOCK_REFERENCE_INFO (one per CLOCK_DOMAIN) --------------------
    /// The CLOCK_DOMAIN descriptors the entity has (others are refused
    /// NO_SUCH_DESCRIPTOR); at most MAX_CLOCK_DOMAINS. Default 1.
    static constexpr size_t MAX_CLOCK_DOMAINS = 4;
    void set_clock_domain_count(size_t count) noexcept;
    [[nodiscard]] auto clock_domain_count() const noexcept -> size_t { return clock_domain_count_; }

    /// The vendor's Default Media Clock Reference Priority (Milan v1.3 Section 7.6.1.1) for every
    /// clock domain; a user priority still at the old default follows it. Default MVU_MCR_PRIO_NETWORK_INTERFACE.
    void set_default_mcr_priority(uint8_t prio) noexcept;
    /// Clock domain @p index's default priority, as above; false past clock_domain_count().
    [[nodiscard]] auto set_default_mcr_priority(size_t index, uint8_t prio) noexcept -> bool;
    /// Clock domain @p index's default priority; MVU_MCR_PRIO_DEFAULT past MAX_CLOCK_DOMAINS.
    [[nodiscard]] auto default_mcr_priority(size_t index) const noexcept -> uint8_t;

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
    [[nodiscard]] auto set_mcr_info(size_t index, McrInfo const& info) noexcept -> bool;

    /// Process an MVU command (atdecc::mvu::is_mvu_command) from @p src_mac, @p payload starting
    /// at the AECP common header, for @p our_entity_id, received at @p now (a binding's time), and
    /// send the response. @return true if a response was sent, false if the frame was ignored
    [[nodiscard]] auto process_packet(
        Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id, sm::TimePoint now) -> bool;

  private:
    /// Answer the Milan command @p command (control data as declared), received at @p now, by its command_type
    [[nodiscard]] auto dispatch_command(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command, sm::TimePoint now) -> bool;
    /// Send @p header with @p body as its command_specific_data, padded to
    /// atdecc::mvu::MVU_MIN_PDU_LENGTH; false when it does not fit an AECPDU
    [[nodiscard]] auto send_pdu(Eui48 const& dest_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> body) const
        -> bool;
    /// Send a response to @p command with @p status and @p body as its
    /// command_specific_data, padded to atdecc::mvu::MVU_MIN_PDU_LENGTH
    [[nodiscard]] auto send_response(
        Eui48 const& dest_mac, atdecc::mvu::MvuDu const& command, uint8_t status, std::span<uint8_t const> body) const -> bool;

    /// Answer @p command (control data as declared) by echoing it with status
    /// NOT_IMPLEMENTED, padded to atdecc::mvu::MVU_MIN_PDU_LENGTH
    [[nodiscard]] auto send_not_implemented(Eui48 const& dest_mac, std::span<uint8_t const> command) const -> bool;

    /// The SET_SYSTEM_UNIQUE_ID command @p command (control data as declared)
    [[nodiscard]] auto set_system_unique_id_command(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command) -> bool;
    /// The SET_MEDIA_CLOCK_REFERENCE_INFO command @p command
    [[nodiscard]] auto set_mcr_info_command(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command) -> bool;
    /// The GET_MEDIA_CLOCK_REFERENCE_INFO command @p command
    [[nodiscard]] auto get_mcr_info_command(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command) -> bool;
    /// SET_MEDIA_CLOCK_REFERENCE_INFO's @p request (a valid clock domain) applied and answered
    [[nodiscard]] auto apply_mcr_info(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, atdecc::mvu::MvuMediaClockReferenceInfoPayload const& request)
        -> bool;
    /// Answer @p header with SUCCESS and @p body, then notify_changed() if @p changed
    [[nodiscard]] auto respond_and_notify(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> body, bool changed) -> bool;
    /// Whether SET_SYSTEM_UNIQUE_ID may set @p number in the profile's version
    [[nodiscard]] auto system_unique_id_settable(uint64_t number) const noexcept -> bool;
    /// Set the id to @p number and @p name (kept from v1.3, well-formed UTF-8); true when it changed
    [[nodiscard]] auto replace_system_unique_id(uint64_t number, std::array<char, 64> const& name) noexcept -> bool;
    /// SET_SYSTEM_UNIQUE_ID's requested id @p number and @p name applied and answered
    [[nodiscard]] auto apply_system_unique_id(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, uint64_t number, std::array<char, 64> const& name) -> bool;
    /// The system unique id as a response in the profile's layout carries it, at the head of @p out
    [[nodiscard]] auto system_unique_id_payload(std::span<uint8_t> out) const noexcept -> std::span<uint8_t const>;
    /// GET_MILAN_INFO's response in the profile's layout, at the head of @p out
    [[nodiscard]] auto milan_info_payload(std::span<uint8_t> out) const noexcept -> std::span<uint8_t const>;

    /// BIND_STREAM, UNBIND_STREAM (Milan v1.3 Sections 5.4.4.6-7) at @p now, and GET_STREAM_INPUT_INFO_EX (5.4.4.8)
    [[nodiscard]] auto bind_stream_command(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command, sm::TimePoint now) -> bool;
    [[nodiscard]] auto unbind_stream_command(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command, sm::TimePoint now) -> bool;
    /// A binding command @p command, a @p Payload, at @p now: refused by its descriptor, else @p bind
    /// (the sink's index, the payload, the time, the listener's reply function) run through run_binding.
    template <typename Payload, typename Bind>
    [[nodiscard]] auto binding_command(
        Eui48 const& src_mac,
        atdecc::mvu::MvuDu const& header,
        std::span<uint8_t const> command,
        sm::TimePoint now,
        Bind const& bind) -> bool;
    [[nodiscard]] auto stream_input_info_ex_command(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> command) -> bool;
    /// Whether STREAM_INPUT @p index exists to bind (a sink, and a descriptor of the model)
    [[nodiscard]] auto bindable(uint16_t index) const noexcept -> bool;
    /// What a command addressing STREAM_INPUT @p index of type @p type is refused with, or nullopt
    [[nodiscard]] auto stream_input_refusal(uint16_t type, uint16_t index) const noexcept -> std::optional<uint8_t>;
    /// Sink @p index's GET_STREAM_INPUT_INFO_EX payload (a known sink)
    [[nodiscard]] auto stream_input_info_ex(uint16_t index) const noexcept -> atdecc::mvu::MvuStreamInputInfoExPayload;
    /// Runs one binding on the Milan listener, giving it the reply; its result
    using BindingFn = sg14::inplace_function<bool(atdecc::MilanListener::ReplyFn const&), 64>;
    /// Run @p binding on known sink @p index, answering @p header with @p body when the listener replies
    /// (ENTITY_MISBEHAVING if it never does), and notifying the others of a change.
    [[nodiscard]] auto run_binding(
        Eui48 const& src_mac,
        atdecc::mvu::MvuDu const& header,
        std::span<uint8_t const> body,
        uint16_t index,
        BindingFn const& binding) -> bool;
    /// Answer a binding command @p header with the MVU status for @p acmp_status and @p body, then,
    /// when it @p changed the binding, notify the other controllers.
    [[nodiscard]] auto answer_binding(
        Eui48 const& src_mac, atdecc::mvu::MvuDu const& header, std::span<uint8_t const> body, uint8_t acmp_status, bool changed)
        -> bool;
    /// Clock domain @p index (< MAX_CLOCK_DOMAINS) takes default priority @p prio
    void apply_default_mcr_priority(size_t index, uint8_t prio) noexcept;
    /// Clock domain @p index's MCR payload, as a response carries it
    [[nodiscard]] auto mcr_payload(uint16_t index) const noexcept -> atdecc::mvu::MvuMediaClockReferenceInfoPayload;
    /// Refuse a SET from @p header's controller while another holds the entity
    [[nodiscard]] auto locked_against(atdecc::mvu::MvuDu const& header) const noexcept -> bool;
    /// Send @p body as an unsolicited @p command_type response (u bit set) to
    /// every registered controller but @p requester, then report the change
    void notify_changed(uint16_t command_type, Eui64 const& requester, std::span<uint8_t const> body);
    /// Send @p body as an unsolicited @p command_type response to every registered controller but
    /// @p requester (Milan v1.3 Section 5.4.5.2)
    void send_unsolicited(uint16_t command_type, Eui64 const& requester, std::span<uint8_t const> body);

    EntityProfile profile_{};
    uint32_t certification_version_{0};
    uint32_t extra_features_{0};
    SendAemResponseFn send_response_{};
    AemCommandHandler* aem_{nullptr};
    atdecc::MilanListener* listener_{nullptr};
    std::optional<size_t> stream_input_count_{};
    MvuSettingsChangedFn on_settings_changed_{};
    Eui64 our_entity_id_{};
    uint64_t system_unique_id_{0};
    std::array<char, 64> system_unique_name_{};
    std::array<uint8_t, MAX_CLOCK_DOMAINS> default_mcr_prio_{[] {
        std::array<uint8_t, MAX_CLOCK_DOMAINS> prio{};
        prio.fill(atdecc::mvu::MVU_MCR_PRIO_NETWORK_INTERFACE);
        return prio;
    }()};
    size_t clock_domain_count_{1};
    std::array<McrInfo, MAX_CLOCK_DOMAINS> mcr_{};
};

/// Route one AECPDU addressed to the entity, received at @p now (the handlers' event time): an MVU
/// command to @p mvu, anything else to @p aem. @return the handler's result: true if a response was sent
[[nodiscard]] auto dispatch_aecp(
    AemCommandHandler& aem,
    MvuCommandHandler& mvu,
    Eui48 const& src_mac,
    std::span<uint8_t const> payload,
    Eui64 const& our_entity_id,
    sm::TimePoint now) -> bool;

}  // namespace statusbar::nanoavb
