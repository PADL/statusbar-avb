#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB Entity - AEM command/response handler
/// Handles incoming AECP AEM commands using the EntityModel

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/container/container_slot_table.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/nanoavb/nanoavb_dynamic_audio_maps.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model_adapter.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::Eui64;
using ieee::quadlet_t;
using namespace atdecc;
using namespace atdecc::aem;
using statusbar::container::SlotTable;

//
// AEM Command Handler Response
//
/// Maximum size for AEM response data: what one Ethernet frame carries after the AEM header
/// (1500 - 24 octets), which Milan v1.3 Section 5.4.1 lets a GET_AUDIO_MAP response use.
/// Used by process_packet() to allocate one stack-local output buffer that
/// is passed into the per-command handlers.
constexpr size_t MAX_AEM_RESPONSE_SIZE = 1476;

/// The most AEM command-specific data one AECPDU can carry (IEEE 1722.1-2021 Clause 9.2.1.1.7).
constexpr size_t MAX_AEM_BODY = AECP_MAX_CONTROL_DATA_LENGTH - AemDu::AEM_DATA_LENGTH;

/// Small value returned by each AEM command handler.
///
/// Handlers write their response bytes into a caller-supplied
/// `std::span<uint8_t>` and return just the status code plus the number
/// of bytes they wrote. Previously each handler returned a 1040-byte
/// struct containing an inline std::array<uint8_t, MAX_AEM_RESPONSE_SIZE>,
/// which the stub handlers (NOT_IMPLEMENTED) paid for even though they
/// produced zero bytes. This shape limits the per-dispatch stack cost
/// to one MAX_AEM_RESPONSE_SIZE buffer in process_packet(), regardless
/// of how many handlers are traversed.
struct AemCommandResponse
{
    uint8_t status{AEM_STATUS_SUCCESS};
    size_t size{0};  ///< Bytes written to the caller's output buffer
};

//
// AEM Command Handler Callbacks
//
using SendAemResponseFn = statusbar::sg14::inplace_function<bool(Eui48 const& dest_mac, std::span<uint8_t const> response), 64>;

/// Fill the IEEE 1722.1 counters block for a descriptor (Clause 7.4.42).
/// Counters are dynamic runtime state (stream reception health, etc.) so they
/// come from the application, not the static AEM descriptor model. On a descriptor
/// that has counters, set @p counters_valid (bit i set => counter i present) and
/// @p counters[i], and return true; return false for a descriptor with no
/// counters (the handler then replies NO_SUCH_DESCRIPTOR).
using GetCountersFn = statusbar::sg14::inplace_function<
    bool(uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& counters_valid, std::array<uint32_t, 32>& counters),
    64>;

/// Fill the IEEE 1722.1 GET_STREAM_INFO response (Clause 7.4.16) for a stream
/// descriptor. The handler pre-fills @p out.descriptor_type / descriptor_index;
/// the application fills the dynamic stream parameters (stream_id, stream_format,
/// stream_dest_mac, stream_vlan_id, msrp_accumulated_latency) and sets the
/// matching `*_VALID` / CONNECTED flag bits, then returns true. Return false for
/// a descriptor that is not one of this entity's streams (the handler then replies
/// NO_SUCH_DESCRIPTOR). Optional; if unset, GET_STREAM_INFO replies NOT_IMPLEMENTED.
///
/// Milan listeners (e.g. the DSP processor) query GET_STREAM_INFO on a talker's
/// STREAM_OUTPUT after connecting to verify the stream's format/identity, and
/// tear the connection down if the talker answers NOT_IMPLEMENTED.
using GetStreamInfoFn =
    statusbar::sg14::inplace_function<bool(uint16_t descriptor_type, uint16_t descriptor_index, AemStreamInfoPayload& out), 64>;

/// One MSRP mapping of a GET_AVB_INFO response (IEEE 1722.1 7.4.40.2): the
/// traffic class, priority code point and VLAN of one SR class.
///
/// traffic_class carries the MSRP SRclassID (IEEE 802.1Q 35.2.2.9.2: 6 for
/// class A, 5 for class B), not a queue number. Table 7-149 says only "the
/// traffic class" and Milan adds nothing, so the value follows what the
/// devices and controllers in the field exchange: Milan and macOS entities
/// answer 6 and 5, and a controller matching the mapping against the MSRP
/// Domain attribute expects the same number it sees there.
struct AvbInfoMsrpMapping
{
    uint8_t traffic_class{0};
    uint8_t priority{0};
    uint16_t vlan_id{0};
};

/// What GET_AVB_INFO reports for one AVB_INTERFACE (7.4.40): the live gPTP
/// state (grandmaster, propagation delay, domain, flags) and the MSRP mappings.
struct AvbInfo
{
    static constexpr size_t MAX_MSRP_MAPPINGS = 8;

    Eui64 gptp_grandmaster_id{};
    uint32_t propagation_delay{0};  ///< nanoseconds
    uint8_t gptp_domain_number{0};
    uint8_t flags{0};  ///< aem::avb_info_flags
    statusbar::sg14::inplace_vector<AvbInfoMsrpMapping, MAX_MSRP_MAPPINGS> msrp_mappings{};
};

/// Fill the GET_AVB_INFO answer for AVB_INTERFACE @p descriptor_index and
/// return true; return false for an index that is not one of this entity's
/// interfaces (the handler then replies NO_SUCH_DESCRIPTOR). Optional; if
/// unset, GET_AVB_INFO replies NOT_IMPLEMENTED. AvbEntityHost wires this to
/// its gPTP state so entities built on it need not.
using GetAvbInfoFn = statusbar::sg14::inplace_function<bool(uint16_t descriptor_index, AvbInfo& out), 64>;

/// The most clock identities an AS path holds here: the grandmaster, each time-aware
/// bridge, and this system (IEEE 802.1AS-2020 Clause 10.3.9.23).
constexpr size_t MAX_AS_PATH_LENGTH = 32;
/// The clock identities of an AS path, grandmaster first (IEEE 802.1AS-2020 Clause 10.3.9.23).
using AsPath = statusbar::sg14::inplace_vector<Eui64, MAX_AS_PATH_LENGTH>;

/// Fill @p path with the gPTP path of AVB_INTERFACE @p descriptor_index (IEEE 1722.1-2021
/// Clause 7.4.41), empty while no grandmaster is known, and return true; false for an
/// index that is not one of this entity's interfaces (answered NO_SUCH_DESCRIPTOR).
using GetAsPathFn = statusbar::sg14::inplace_function<bool(uint16_t descriptor_index, AsPath& path), 64>;

/// STREAM_OUTPUT @p stream_output_index's presentation time offset in nanoseconds, the
/// max_transit_time of IEEE 1722.1-2021 Clause 7.4.77, or nullopt when it is not one of
/// this entity's talker streams (answered NO_SUCH_DESCRIPTOR).
using GetPresentationOffsetFn = statusbar::sg14::inplace_function<std::optional<uint32_t>(uint16_t stream_output_index), 64>;

/// Put @p offset_ns in force as STREAM_OUTPUT @p stream_output_index's presentation time
/// offset (Clause 7.4.76) and answer an AEM status: SUCCESS once in force, STREAM_IS_RUNNING
/// when it cannot change while the stream runs, NOT_SUPPORTED when the entity cannot change it,
/// or IN_PROGRESS when it needs time, to finish with complete_pending_command() as a deferred SET.
using SetPresentationOffsetFn = statusbar::sg14::inplace_function<uint8_t(uint16_t stream_output_index, uint32_t offset_ns), 64>;

/// Put stream port (@p port_type, @p port_index)'s new dynamic mappings (all of them, in cluster
/// order) in force in the data plane: SUCCESS once in force, any other AEM status refuses and leaves
/// the port as it was. Called on the reactor thread before the response; it must not change mappings
/// itself. An entity that does not change a streaming STREAM_OUTPUT's mappings refuses BAD_ARGUMENTS
/// (Milan v1.3 Section 5.4.2.27, without TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING).
using AudioMappingsChangedFn = statusbar::sg14::
    inplace_function<uint8_t(uint16_t port_type, uint16_t port_index, std::span<DynamicAudioMapping const> mappings), 64>;

/// Notified when a SET_CONTROL targeting the entity's IDENTIFY control is
/// applied (see set_identify_control_index). @p active is true when the new
/// value is non-zero (a controller is identifying this entity), false when
/// cleared.
using IdentifyChangedFn = statusbar::sg14::inplace_function<void(bool active), 64>;

/// Told the system unique id and name in force (Milan v1.3 Section 5.4.4.2) after an accepted
/// SET_SYSTEM_UNIQUE_ID changed either, before its response is sent, for the entity to save them.
/// Called on the reactor thread; it must not throw. A later install replaces an earlier one.
using SystemUniqueIdChangedFn = statusbar::sg14::inplace_function<void(MvuSystemUniqueIdPayload const& in_force), 64>;

/// REBOOT (IEEE 1722.1 7.4.43): decide whether to reboot the target named by
/// @p descriptor_type / @p descriptor_index (normally ENTITY 0) and return an
/// AEM status. The callback runs BEFORE the response and the unsolicited
/// notification are sent, so it must schedule the restart (a timer, or a flag
/// the event loop acts on after this packet) rather than restart synchronously.
/// Optional; if unset, REBOOT replies NOT_IMPLEMENTED.
using RebootFn = statusbar::sg14::inplace_function<uint8_t(uint16_t descriptor_type, uint16_t descriptor_index), 64>;

/// Callbacks for AECP AEM command handling
struct AemCommandHandlerCallbacks
{
    /// Send AECP AEM response to the requesting controller (unicast)
    /// @param dest_mac Destination MAC address (controller that sent the command)
    /// @param response Complete response packet (AemDu header + command-specific data)
    /// @return true if response was sent successfully
    SendAemResponseFn send_response;

    /// Provide counters for GET_COUNTERS. Optional; if unset, GET_COUNTERS replies
    /// NOT_IMPLEMENTED.
    GetCountersFn get_counters;

    /// Provide stream parameters for GET_STREAM_INFO. Optional; if unset,
    /// GET_STREAM_INFO replies NOT_IMPLEMENTED.
    GetStreamInfoFn get_stream_info;

    /// Provide the live gPTP/MSRP state for GET_AVB_INFO. Optional; if unset,
    /// GET_AVB_INFO replies NOT_IMPLEMENTED.
    GetAvbInfoFn get_avb_info;

    /// Provide the gPTP path for GET_AS_PATH. Optional; if unset, GET_AS_PATH
    /// replies NOT_IMPLEMENTED.
    GetAsPathFn get_as_path;

    /// Observe IDENTIFY-control changes (e.g. blink an LED, or log when there
    /// is none). Optional.
    IdentifyChangedFn identify_changed;

    /// Accept or refuse REBOOT, and schedule it. Optional; if unset, REBOOT
    /// replies NOT_IMPLEMENTED.
    RebootFn on_reboot;

    /// The talkers' presentation time offsets, for SET_MAX_TRANSIT_TIME and
    /// GET_MAX_TRANSIT_TIME. Both or neither; unset, those reply NOT_IMPLEMENTED.
    GetPresentationOffsetFn get_presentation_offset;
    SetPresentationOffsetFn set_presentation_offset;

    /// Put a stream port's dynamic mappings in force. Optional; if unset,
    /// ADD_AUDIO_MAPPINGS and REMOVE_AUDIO_MAPPINGS reply NOT_IMPLEMENTED.
    AudioMappingsChangedFn on_audio_mappings_changed;
};

/// What answers Milan's stream input commands over AECP MVU (Milan v1.3
/// 5.4.4.6 to 5.4.4.8): the entity's Milan connection management, when it
/// runs one. The handler reads the commands and writes the responses; what
/// a sink does with them is asked here.
struct MvuStreamInputCallbacks
{
    /// Takes the ACMP response a sink gives to a bind or unbind.
    using Respond = statusbar::sg14::inplace_function<void(atdecc::AcmpCommandResponse const&), 64>;

    /// A BIND_STREAM or UNBIND_STREAM command, as the BIND_RX or UNBIND_RX
    /// command it equals (Table 5.22). @p respond is called once, at the
    /// step where the sink would send its ACMP response (5.5.3.2).
    statusbar::sg14::inplace_function<void(atdecc::AcmpCommandResponse const& command, Respond const& respond), 64> binding_command;

    /// The GET_STREAM_INPUT_INFO_EX response data of STREAM_INPUT @p index,
    /// nullopt when the entity has no such stream input.
    statusbar::sg14::inplace_function<std::optional<atdecc::MvuGetStreamInputInfoExResponse>(uint16_t index), 64> stream_input_info;

    /// The binding parameters of STREAM_INPUT @p index (5.3.8.3), nullopt
    /// while it is not bound or the entity has no such stream input. It is
    /// how the handler knows that a BIND_STREAM or UNBIND_STREAM changed the
    /// entity's state, which is when 5.4.5.3 has the command notified to the
    /// other registered controllers. Unset, none is notified.
    statusbar::sg14::inplace_function<std::optional<atdecc::MilanBinding>(uint16_t index), 64> binding;
};

/// Parameters for building acquire/lock response packets
/// Uses designated initializers for self-documenting call sites
struct AcquireLockResponseParams
{
    uint8_t status;                         ///< AEM status code
    uint32_t flags;                         ///< Response flags (network byte order conversion handled internally)
    Eui64 const& entity_id;                 ///< Owner (acquire) or locker (lock) entity ID
    std::span<uint8_t const> command_data;  ///< Original command data to echo descriptor info
};

//
// AEM Command Handler
//
/// AemCommandHandler — processes incoming AECP AEM commands using the
/// EntityModel. Does not do packet I/O itself; the caller injects packets
/// via `process_packet()` and supplies a `send_response` callback.
///
/// Thread model: all methods are single-threaded. The caller must serialize
/// access to a single AemCommandHandler instance — `process_packet()`,
/// `tick()`, `controller_available_response_received()`,
/// `controller_available_timed_out()`, and the state-query accessors
/// (`is_acquired()`, `is_locked()`, etc.) must not be invoked concurrently
/// from multiple threads. The typical deployment has one thread driving
/// a reactor that calls `process_packet` on inbound frames and `tick` on
/// a timer, both on the same thread.
class AemCommandHandler
{
  public:
    /// Construct from a legacy vector-backed `EntityModel`.
    ///
    /// Internally wraps the `EntityModel` in an `EntityModelAdapter`
    /// (stored in `adapter_storage_`) that implements `AemEntityHandler`,
    /// so READ_DESCRIPTOR responses still flow through the new
    /// handler-based dispatch path. This constructor exists for backward
    /// compatibility with the many call sites that still build an
    /// EntityModel — tests, nanoavb_components, nanoavb_example — and
    /// will be deleted in Phase 5 once everything migrates to
    /// `AemEntityHandler` directly.
    /// @param model Reference to the entity model for descriptor lookups.
    explicit AemCommandHandler(EntityModel& model)
        : adapter_storage_{std::in_place, model}
        , handler_{&*adapter_storage_}
    {}

    /// Construct from a user-supplied `AemEntityHandler`. This is the
    /// new-world constructor: the caller owns the handler (typically a
    /// `DescriptorStorageHandler` subclass that serves a compiled-in
    /// .aem blob plus runtime-patched fields) and AemCommandHandler
    /// stores just a pointer. No legacy EntityModel involved.
    /// @param handler Reference to the application's AemEntityHandler
    ///                implementation. Must outlive this object.
    explicit AemCommandHandler(AemEntityHandler& handler)
        : handler_{&handler}
    {}

    // Self-referential: `handler_` may point into `adapter_storage_`, so a
    // default move/copy would leave `handler_` dangling at the source object
    // (and the adapter still referencing the source's EntityModel). The type
    // is constructed in place — as a member of the non-movable
    // NanoAvbComponents — and never relocated.
    AemCommandHandler(AemCommandHandler const&) = delete;
    auto operator=(AemCommandHandler const&) -> AemCommandHandler& = delete;
    AemCommandHandler(AemCommandHandler&&) = delete;
    auto operator=(AemCommandHandler&&) -> AemCommandHandler& = delete;

    /// Set the callbacks for sending responses
    /// @param callbacks The callback interface for sending AEM responses
    void set_callbacks(AemCommandHandlerCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Set only the GET_COUNTERS provider, without disturbing the others
    /// (set_callbacks replaces the whole struct, which would clear send_response
    /// wired by the net layer). The application owns the dynamic counters.
    void set_send_response(SendAemResponseFn fn) { callbacks_.send_response = std::move(fn); }
    void set_get_counters(GetCountersFn fn) { callbacks_.get_counters = std::move(fn); }

    /// Emit descriptors at their IEEE 1722.1-2013/2016 wire sizes in
    /// READ_DESCRIPTOR responses (truncate the 2021-only tail fields). Set
    /// for controllers that reject 2021-length descriptors. Default off
    /// (2021 lengths).
    void set_legacy_2016(bool enable) noexcept { legacy_2016_ = enable; }
    /// Whether descriptors go out at their 1722.1-2013/2016 lengths.
    [[nodiscard]] auto legacy_2016() const noexcept -> bool { return legacy_2016_; }

    /// Let a SET_CONTROL of the entity's IDENTIFY control through while another
    /// controller holds the entity acquired or locked. Off by default: IEEE
    /// 1722.1-2021 7.4.25.2 has a held entity answer every other controller's
    /// SET_CONTROL with ENTITY_ACQUIRED or ENTITY_LOCKED, IDENTIFY included, and
    /// Milan v1.3 5.4.2.17 asks the same. On, any controller can make the box
    /// show itself while it is held, which some installations prefer.
    void set_identify_bypasses_exclusive_access(bool const enable) noexcept { identify_bypasses_exclusive_access_ = enable; }

    /// The specification the entity answers to where IEEE 1722.1-2021 and
    /// Milan v1.3 differ. Under classic, 7.5.2 is read as written: every
    /// SET_* answered SUCCESS is followed by an unsolicited notification.
    /// Under milan13, 5.4.5.3 asks for a notification when a command modifies
    /// the entity's state, so a SET to the value already in force is answered
    /// SUCCESS without a notification, and without reaching the hook, since
    /// there is nothing to apply.
    enum class Profile : uint8_t
    {
        classic,
        milan13,
    };
    void set_profile(Profile const profile) noexcept { profile_ = profile; }
    [[nodiscard]] auto profile() const noexcept -> Profile { return profile_; }

    /// The GET_STREAM_INFO response length in octets: IEEE 1722.1-2021's 84 under milan13
    /// (Milan v1.3 Section 5.4.2.10), 1722.1-2013's 48 under classic, for older controllers.
    [[nodiscard]] auto stream_info_length() const noexcept -> size_t
    {
        return profile_ == Profile::milan13 ? AEM_STREAM_INFO_2021_LENGTH : AemStreamInfoPayload::LENGTH;
    }

    /// What a Milan PAAD-AE says of itself in GET_MILAN_INFO (Milan v1.3
    /// 5.4.4.1), for the currently active Configuration. Set, the entity is a
    /// Milan device: it answers the MVU commands (GET_MILAN_INFO with these
    /// fields, the stream input commands when attach_mvu_stream_inputs() has
    /// given it something to answer them with, the rest NOT_IMPLEMENTED).
    /// Unset, it is a plain 1722.1 entity
    /// and lets every MVU command pass unanswered, as 5.4.4.1 recommends of
    /// a Configuration that is not Milan.
    struct MilanInfo
    {
        uint32_t features_flags{0};                                 ///< Table 5.17
        uint32_t certification_version{0};                          ///< mvu_version(); 0 for none passed
        uint32_t specification_version{atdecc::mvu_version(1, 3)};  ///< the Milan version supported
        auto operator==(MilanInfo const&) const noexcept -> bool = default;
    };
    void set_milan_info(std::optional<MilanInfo> const info) noexcept { milan_info_ = info; }
    [[nodiscard]] auto milan_info() const noexcept -> std::optional<MilanInfo> const& { return milan_info_; }

    /// The CLOCK_DOMAINs whose media clock reference settings the entity keeps (Milan v1.3
    /// Section 7.6); MEDIA_CLOCK_REFERENCE_INFO for a later one is NO_SUCH_DESCRIPTOR.
    static constexpr size_t MAX_CLOCK_DOMAINS = 4;

    /// The vendor's Default Media Clock Reference Priority of CLOCK_DOMAIN @p clock_domain_index
    /// (Milan v1.3 Section 7.6.1.1, Table 7.3), MVU_MCR_PRIO_DEFAULT until set; a user priority
    /// no controller has set follows it. False past MAX_CLOCK_DOMAINS.
    [[nodiscard]] auto set_default_mcr_priority(uint16_t clock_domain_index, uint8_t prio) noexcept -> bool;

    /// What GET_MEDIA_CLOCK_REFERENCE_INFO answers for CLOCK_DOMAIN @p clock_domain_index (Milan
    /// v1.3 Section 5.4.4.5), whether or not the model has it; the defaults past MAX_CLOCK_DOMAINS.
    [[nodiscard]] auto media_clock_reference_info(uint16_t clock_domain_index) const noexcept -> MvuMediaClockReferenceInfoPayload;

    /// What GET_SYSTEM_UNIQUE_ID answers (Milan v1.3 Section 5.4.4.3): the id and name last set or
    /// restored, 0 and empty until then, the reserved octets and those after the name zero.
    [[nodiscard]] auto system_unique_id() const noexcept -> MvuSystemUniqueIdPayload
    {
        return {.system_unique_id = system_unique_id_, .system_name = system_name_};
    }
    /// Put back the system unique id @p id and name @p name the entity saved (Milan v1.3 Section
    /// 5.4.4.2): on the reactor thread, before the entity starts. Notifies nobody and calls no
    /// hook. False, and nothing changed, for a name that is not well-formed UTF-8.
    [[nodiscard]] auto restore_system_unique_id(uint64_t id, aem::AtdeccString const& name) noexcept -> bool;
    /// Install the hook told each id and name a SET_SYSTEM_UNIQUE_ID puts in force (SystemUniqueIdChangedFn).
    void set_on_system_unique_id_changed(SystemUniqueIdChangedFn fn) { on_system_unique_id_changed_ = std::move(fn); }

    /// Give the handler what answers BIND_STREAM, UNBIND_STREAM and
    /// GET_STREAM_INPUT_INFO_EX, on behalf of @p owner, an address that names
    /// whoever attaches (a net handler passes itself). Without it the three
    /// answer NOT_IMPLEMENTED, which is what an entity running IEEE 1722.1
    /// connection management has to say of them. detach_mvu_stream_inputs()
    /// takes them away again, and leaves alone those a later owner attached.
    void attach_mvu_stream_inputs(void const* const owner, MvuStreamInputCallbacks callbacks)
    {
        mvu_stream_inputs_ = std::move(callbacks);
        mvu_stream_inputs_owner_ = owner;
    }
    void detach_mvu_stream_inputs(void const* const owner) noexcept
    {
        if (mvu_stream_inputs_owner_ == owner) {
            mvu_stream_inputs_ = {};
            mvu_stream_inputs_owner_ = nullptr;
        }
    }

    /// The time of the event being handled, as set_event_time() gave it.
    [[nodiscard]] auto event_time() const noexcept -> sm::TimePoint { return event_time_; }

    /// Set only the GET_STREAM_INFO provider, without disturbing the others.
    /// The application owns the dynamic per-stream parameters.
    void set_get_stream_info(GetStreamInfoFn fn) { callbacks_.get_stream_info = std::move(fn); }
    void set_identify_changed(IdentifyChangedFn fn) { callbacks_.identify_changed = std::move(fn); }
    void set_get_avb_info(GetAvbInfoFn fn) { callbacks_.get_avb_info = std::move(fn); }
    void set_get_as_path(GetAsPathFn fn) { callbacks_.get_as_path = std::move(fn); }
    void set_on_reboot(RebootFn fn) { callbacks_.on_reboot = std::move(fn); }
    /// Install the presentation time offset hooks (see AemCommandHandlerCallbacks).
    void set_presentation_offset_hooks(GetPresentationOffsetFn get, SetPresentationOffsetFn set)
    {
        callbacks_.get_presentation_offset = std::move(get);
        callbacks_.set_presentation_offset = std::move(set);
    }

    // ---- Dynamic audio mappings (IEEE 1722.1-2021 Clauses 7.4.44-7.4.46) --------
    // A STREAM_PORT without AUDIO_MAP descriptors has dynamic mappings, kept in the store the
    // entity hands the handler. A configuration switch forgets them.

    /// A GET_AUDIO_MAP partition under milan13: 176 mappings, 1420 octets (Milan v1.3 Section 5.4.1).
    static constexpr size_t MILAN_AUDIO_MAP_MAPPINGS = 176;
    /// A GET_AUDIO_MAP partition under classic, within an AECPDU's 524 octets of control data.
    static constexpr size_t AUDIO_MAP_MAPPINGS = (MAX_AEM_BODY - AemAudioMapResponseHeader::LENGTH) / AemAudioMapping::LENGTH;
    static_assert(
        AemAudioMapResponseHeader::LENGTH + (MILAN_AUDIO_MAP_MAPPINGS * AemAudioMapping::LENGTH) <= MAX_AEM_RESPONSE_SIZE);
    /// The STREAM_OUTPUTs an output port's GET_AUDIO_MAP partitions count (Milan v1.3 Section 5.4.2.26).
    static constexpr size_t MAX_PARTITIONED_STREAMS = 256;

    /// Give the handler the store of its stream ports' dynamic mappings, both directions; the
    /// handler alone changes it. Without one (the default) GET_AUDIO_MAP, ADD_AUDIO_MAPPINGS and
    /// REMOVE_AUDIO_MAPPINGS answer NOT_IMPLEMENTED. @p maps must outlive the handler, or be withdrawn.
    void set_dynamic_audio_maps(DynamicAudioMaps* maps) noexcept { audio_maps_ = maps; }
    /// Install the data plane's hook for the dynamic mappings (AudioMappingsChangedFn).
    void set_on_audio_mappings_changed(AudioMappingsChangedFn fn) { callbacks_.on_audio_mappings_changed = std::move(fn); }
    /// The store of the dynamic mappings in force, nullptr without one.
    [[nodiscard]] auto dynamic_audio_maps() const noexcept -> DynamicAudioMaps const* { return audio_maps_; }
    /// Whether STREAM_PORT_INPUT or STREAM_PORT_OUTPUT (@p port_type, @p port_index) of the current
    /// configuration has dynamic mappings (no AUDIO_MAP descriptor).
    [[nodiscard]] auto has_dynamic_map(uint16_t port_type, uint16_t port_index) const -> bool;
    /// Make @p mappings dynamic port (@p port_type, @p port_index)'s, checked as an ADD onto an empty
    /// port, without the hook and without notifying: an entity's defaults, or mappings it restores to
    /// a data plane it has set itself. The AEM status; nothing changes unless SUCCESS.
    [[nodiscard]] auto seed_audio_mappings(uint16_t port_type, uint16_t port_index, std::span<DynamicAudioMapping const> mappings)
        -> uint8_t;
    /// The entity's own REMOVE of @p remove (a mapping not held ignored), then ADD of @p add, on
    /// dynamic port (@p port_type, @p port_index), all or nothing, checked and put in force through the
    /// hook as a controller's would be; a change is notified to every registered controller as REMOVE
    /// then ADD_AUDIO_MAPPINGS, in AECPDUs of at most 63 mappings. The AEM status; local_change_refusal()
    /// while it refuses.
    [[nodiscard]] auto apply_local_audio_mappings(
        uint16_t port_type,
        uint16_t port_index,
        std::span<DynamicAudioMapping const> add,
        std::span<DynamicAudioMapping const> remove) -> uint8_t;
    /// Whether every mapping held still names a cluster channel of its port and a channel of its
    /// stream's current format at its unit's rate, as an ADD of it would be checked; the handler
    /// keeps it so by refusing a stream format or rate that would break one (mappings_refusal()).
    [[nodiscard]] auto mappings_hold() const -> bool;
    /// The status SET_STREAM_FORMAT or SET_SAMPLING_RATE @p command_body is refused with because a
    /// mapping held would not hold after it: BAD_ARGUMENTS for a stream channel or rate a format
    /// leaves (Milan v1.3 Sections 5.4.2.7, 5.4.2.27), NOT_SUPPORTED for a rate (Section 5.4.2.13).
    [[nodiscard]] auto mappings_refusal(uint16_t command_type, std::span<uint8_t const> command_body) const
        -> std::optional<uint8_t>;

    /// Called when SET_CONFIGURATION asks to switch to a different (valid)
    /// configuration — this is where the application re-shapes its data
    /// plane. Return AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_*
    /// to reject. Without a registered callback a configuration SWITCH is
    /// refused (NOT_SUPPORTED) — the entity must not claim a configuration
    /// its streams are not actually built for; a SET to the current
    /// configuration is always an idempotent SUCCESS. SET_CONFIGURATION is
    /// never deferred: an IN_PROGRESS is answered ENTITY_MISBEHAVING.
    void set_on_configuration_changed(statusbar::sg14::inplace_function<uint8_t(uint16_t /*configuration*/), 64> fn)
    {
        on_configuration_changed_ = std::move(fn);
    }

    /// Callback to send CONTROLLER_AVAILABLE command to the current owner.
    /// Set this to enable the CONTROLLER_AVAILABLE handshake on acquire contention.
    /// Signature: send_controller_available(owner_entity_id) -> bool.
    /// See SendAemResponseFn for the inline-storage capacity rationale;
    /// this callback's captures are similarly small in practice.
    statusbar::sg14::inplace_function<bool(Eui64 const& owner_entity_id), 64> send_controller_available;

    /// Process an incoming AECP packet and send the response: an AEM command
    /// through the AEM dispatch, a VENDOR_UNIQUE command through the Milan
    /// MVU one when the entity has Milan info to answer with.
    /// @param src_mac Source MAC address (for unicast response)
    /// @param payload The AECP packet payload (starting with the AECP common header)
    /// @param our_entity_id Our entity ID (for target validation)
    /// @return true if packet was processed and response sent, false if ignored
    [[nodiscard]] auto process_packet(Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id) -> bool;

    /// Handle an incoming AEM command. Writes response bytes into
    /// `out_buffer` and returns status + bytes-written.
    /// @param header The AEM header
    /// @param command_data Additional command-specific data after the header
    /// @param out_buffer Caller-owned buffer for the response body (at least MAX_AEM_RESPONSE_SIZE bytes)
    [[nodiscard]] auto handle_command(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Check if the entity is currently acquired
    [[nodiscard]] auto is_acquired() const noexcept -> bool { return acquired_; }

    /// Check if the entity is currently locked
    [[nodiscard]] auto is_locked() const noexcept -> bool { return locked_; }

    /// Get the controller that acquired this entity
    [[nodiscard]] auto acquiring_controller() const noexcept -> Eui64 { return acquiring_controller_; }

    /// Get the controller that locked this entity
    [[nodiscard]] auto locking_controller() const noexcept -> Eui64 { return locking_controller_; }

    /// Release acquisition (e.g., on timeout)
    void release_acquisition() noexcept
    {
        acquired_ = false;
        acquired_persistent_ = false;
        acquiring_controller_ = {};
        acquiring_controller_mac_ = {};
    }

    /// Release lock (e.g., on timeout)
    void release_lock() noexcept
    {
        locked_ = false;
        locking_controller_ = {};
    }

    /// Set the current event time, which the lock, the deferred commands and the controller monitor
    /// time what a packet starts from: call it before process_packet(). tick() moves it to its own
    /// time when that is later, so a packet handled without it is timed no earlier than the last tick.
    void set_event_time(sm::TimePoint now) noexcept { event_time_ = now; }

    /// Notify that the current owner responded to CONTROLLER_AVAILABLE (is alive)
    /// Denies the pending acquire request with ENTITY_ACQUIRED.
    void controller_available_response_received();

    /// Notify that CONTROLLER_AVAILABLE timed out (owner is gone)
    /// Grants acquisition to the pending requester.
    void controller_available_timed_out();

    /// Check if there is a pending acquire waiting for CONTROLLER_AVAILABLE
    [[nodiscard]] auto has_pending_acquire() const noexcept -> bool { return pending_acquire_.active; }

    /// Maximum number of unsolicited notification registrations (Milan v1.3
    /// Section 5.3.4.2: at least 16 per AVB interface).
    static constexpr size_t MAX_UNSOLICITED_REGISTRATIONS = 16;

    /// The controller monitor's period under milan13 (Milan v1.3 Section 5.4.5.4): a random time
    /// between these after a registered controller's last AECP command, tick() sends it
    /// CONTROLLER_AVAILABLE, and deregisters it when neither that nor its one retry is answered.
    static constexpr auto CONTROLLER_MONITOR_MIN = std::chrono::seconds(30);
    static constexpr auto CONTROLLER_MONITOR_MAX = std::chrono::seconds(60);

    /// Get the number of active unsolicited registrations
    [[nodiscard]] auto unsolicited_registration_count() const noexcept -> size_t;

    /// Drop any unsolicited-notification registration held by @p controller_entity_id.
    /// Call this when that controller departs (ENTITY_DEPARTING or discovery age-out):
    /// otherwise its registration lingers forever, holding a slot and making us frame
    /// and send notifications to an entity that is gone. Returns the number removed.
    auto remove_unsolicited_registrations_for(Eui64 controller_entity_id) noexcept -> size_t;

    /// Set our own entity_id, used as the target_entity_id of unsolicited
    /// notifications. process_packet() also keeps this current; call this so
    /// notifications emitted before any command is received carry the right id.
    void set_entity_id(Eui64 id) noexcept { our_entity_id_ = id; }

    /// Tell the handler which CONTROL descriptor index is the entity's IDENTIFY
    /// control (IEEE 1722.1 7.3.5.2). When a SET_CONTROL changes that control, the
    /// unsolicited notification is ALSO sent to ATDECC_IDENTIFY_MULTICAST_MAC so any
    /// controller — even one not registered for unsolicited notifications — sees the
    /// identify. Mirror this index into the ADPDU (identify_control_index +
    /// AEM_IDENTIFY_CONTROL_INDEX_VALID). Unset => no identify multicast.
    void set_identify_control_index(uint16_t index) noexcept
    {
        identify_control_index_ = index;
        identify_control_index_valid_ = true;
    }

    /// The CONTROL descriptor index of the entity's IDENTIFY control, when one
    /// has been wired (AvbEntityHost does this automatically for blob-backed
    /// entities whose model carries an IDENTIFY-typed control).
    [[nodiscard]] auto identify_control_index() const noexcept -> std::optional<uint16_t>
    {
        if (!identify_control_index_valid_) {
            return std::nullopt;
        }
        return identify_control_index_;
    }

    /// Drive the entity's OWN identify control (e.g. from a GPIO button, a
    /// front-panel event, or software). Applies the value exactly as a
    /// controller's SET_CONTROL would, then fans out the unsolicited
    /// notifications — every registered controller plus the IDENTIFY multicast —
    /// and fires the identify_changed callback. Call from the entity's event
    /// loop (this class is single-threaded). Returns the AEM status;
    /// AEM_STATUS_NOT_IMPLEMENTED when no identify control has been wired,
    /// local_change_refusal() while it refuses.
    [[nodiscard]] auto set_local_identify(bool on) -> uint8_t;

    /// Apply a descriptor-value change originated by the entity ITSELF (the
    /// developer's code), as if a controller had sent the SET command, then
    /// notify every registered controller via an unsolicited response. This is
    /// how an entity with no physical controller (e.g. a software IDENTIFY
    /// button) drives its own controls. @p command_type is the AEM_COMMAND_*
    /// SET code (SET_CONTROL, SET_NAME, ...); @p command_body is the SET
    /// command body (descriptor_type + descriptor_index + value). Returns the
    /// AEM status from the handler's symbol-keyed set hook; notifications are
    /// emitted only on AEM_STATUS_SUCCESS. local_change_refusal(), with nothing
    /// applied, while it refuses; mappings_refusal() for a stream format or rate.
    [[nodiscard]] auto apply_local_descriptor_value(uint16_t command_type, std::span<uint8_t const> command_body) -> uint8_t;

    /// ENTITY_LOCKED while a controller holds the lock under milan13, else nullopt: a locked
    /// entity's state is not changed by other than ATDECC (Milan v1.3 Sections 5.3.4.1, 5.4.2.17).
    /// What every local change answers; a host asks it before changing state on its own.
    [[nodiscard]] auto local_change_refusal() const noexcept -> std::optional<uint8_t>
    {
        return profile_ == Profile::milan13 && locked_ ? std::optional<uint8_t>{AEM_STATUS_ENTITY_LOCKED} : std::nullopt;
    }

    /// Emit an unsolicited AEM response (U-bit set) carrying @p command_type and
    /// @p body to every registered controller (plus the IDENTIFY multicast when the
    /// change targets the IDENTIFY control). Exposed so an entity can push a
    /// notification it framed itself; apply_local_descriptor_value() is the usual
    /// entry point. No-op if no send_response callback is wired. Raised while a
    /// command is dispatched (from a hook), it is sent after that command's response,
    /// up to MAX_QUEUED_NOTIFICATIONS of them; past those, a side effect goes at once
    /// after the side effects held, and one of the command's type is not sent but
    /// spends each controller's sequence id (Milan v1.3 Section 5.4.5.1), a gap it sees.
    void emit_unsolicited(uint16_t command_type, std::span<uint8_t const> body);

    /// The notifications one command's dispatch holds for after its response, the last
    /// kept for one of the command's own type.
    static constexpr size_t MAX_QUEUED_NOTIFICATIONS = 4;

    /// Emit an unsolicited MVU response (u bit set) carrying @p command_type and @p body to
    /// every registered controller, each with its own MVU sequence id, which advances whether
    /// or not the send succeeds (Milan v1.3 Sections 5.4.5.1, 5.4.5.2). The controller whose
    /// MVU command is being dispatched is left out. No-op for an entity without milan_info()
    /// or a send_response callback.
    void emit_mvu_unsolicited(uint16_t command_type, std::span<uint8_t const> body);

    /// Something Milan v1.3 Table 5.21 names of STREAM_INPUT @p stream_input_index has changed
    /// (its talker, its Probing status, its ACMP status, the registration of its Talker
    /// attribute): send its GET_STREAM_INPUT_INFO_EX response, as the attached stream inputs
    /// give it now, to every registered controller as an MVU notification
    /// (emit_mvu_unsolicited()). False, and nothing sent, when no stream inputs are attached
    /// or they have no such input.
    auto notify_stream_input_info_ex(uint16_t stream_input_index) -> bool;

    /// Work a hook leaves for after its command's response, such as more notifications than
    /// MAX_QUEUED_NOTIFICATIONS holds.
    using AfterResponseFn = statusbar::sg14::inplace_function<void(), 32>;
    /// How many AfterResponseFn one command holds.
    static constexpr size_t MAX_AFTER_RESPONSE = 4;

    /// Run @p fn once the command being dispatched is answered and its held notifications are
    /// sent, in the order given, the command's controller left out of what it notifies. Outside
    /// a command off the wire (a local change, tick()), or past MAX_AFTER_RESPONSE, it runs at
    /// once. Reactor thread.
    void call_after_response(AfterResponseFn fn);

    /// Push an unsolicited GET_COUNTERS response for (@p descriptor_type,
    /// @p descriptor_index) to the registered controllers. IEEE 1722.1 7.5.2 has
    /// the entity notify when counters change (stream reception, SRP and gPTP
    /// events) but says nothing about rate, so the application decides when to
    /// call this: on a state change such as MEDIA_LOCKED, or at most a few times
    /// a second per descriptor. Counters are read through the get_counters
    /// callback. Returns the AEM status: NOT_IMPLEMENTED without a callback,
    /// NO_SUCH_DESCRIPTOR when the callback declines.
    [[nodiscard]] auto notify_counters_changed(uint16_t descriptor_type, uint16_t descriptor_index) -> uint8_t;

    /// Push an unsolicited GET_AVB_INFO response for AVB_INTERFACE
    /// @p descriptor_index (IEEE 1722.1 7.5.2: a grandmaster, asCapable or
    /// propagation-delay change). Reads the state through the get_avb_info
    /// callback; same return values as notify_counters_changed().
    [[nodiscard]] auto notify_avb_info_changed(uint16_t descriptor_index) -> uint8_t;

    /// Push an unsolicited GET_AS_PATH response for AVB_INTERFACE @p descriptor_index
    /// (IEEE 1722.1-2021 Clauses 7.4.41, 7.5.2: the gPTP path changed). Reads it through
    /// the get_as_path callback; same return values as notify_counters_changed().
    [[nodiscard]] auto notify_as_path_changed(uint16_t descriptor_index) -> uint8_t;

    /// Push an unsolicited GET_STREAM_INFO response for a stream descriptor
    /// (talker advertise/failure, listener connection changes). Reads the
    /// parameters through the get_stream_info callback; same return values as
    /// notify_counters_changed().
    [[nodiscard]] auto notify_stream_info_changed(uint16_t descriptor_type, uint16_t descriptor_index) -> uint8_t;

    // ---- Deferred commands (IN_PROGRESS) ------------------------------------
    //
    // When on_set_descriptor_value returns AEM_STATUS_IN_PROGRESS (e.g. a
    // SET_STREAM_FORMAT that must wait for an SRP re-reservation, Cor 1-2025
    // 7.4.9.3) the handler answers IN_PROGRESS, repeats it every
    // AEM_IN_PROGRESS_TIMEOUT_MS from tick(), and holds the command until the
    // application calls complete_pending_command(). Several commands can be in
    // flight at once (the corrigendum recommends accepting the SET_STREAM_FORMAT
    // of every stream together so one re-reservation wait covers them all).

    /// Deferred commands held at once. A value SET arriving while every slot
    /// is taken is refused NO_RESOURCES before its hook runs, so no work is
    /// begun that could not be finished through complete_pending_command().
    static constexpr size_t MAX_PENDING_COMMANDS = 8;
    /// Longest command body a deferred command keeps for its echoed responses: any an AECPDU
    /// carries, so no deferral is refused for its length after its hook has begun its work.
    static constexpr size_t MAX_PENDING_BODY = MAX_AEM_BODY;
    /// How long a deferred command waits for complete_pending_command() before
    /// tick() answers it PENDING_COMMAND_EXPIRED_STATUS with the value in force
    /// and frees its slot. Twice the SRP re-registration the corrigendum
    /// anticipates (Cor 1-2025 7.4.9.3: two LeaveAll periods, about 30 s), so
    /// a hook that is still working is never cut short, and one that never
    /// finishes does not hold its slot and its controller for ever.
    static constexpr uint32_t PENDING_COMMAND_DEADLINE_MS = 60'000;
    /// What an expired deferred command is answered: whatever it was waiting
    /// for (a reservation, the application) was not had in time.
    static constexpr uint8_t PENDING_COMMAND_EXPIRED_STATUS = AEM_STATUS_NO_RESOURCES;

    /// Finish the deferred (@p command_type, @p descriptor_type, @p descriptor_index):
    /// send its final response with @p status and @p response_body (the echoed
    /// command when empty) and, on SUCCESS, the unsolicited notification. Returns
    /// false when no such command is pending.
    [[nodiscard]] auto complete_pending_command(
        uint16_t command_type,
        uint16_t descriptor_type,
        uint16_t descriptor_index,
        uint8_t status,
        std::span<uint8_t const> response_body = {}) -> bool;

    /// Number of deferred commands awaiting complete_pending_command().
    [[nodiscard]] auto pending_command_count() const noexcept -> size_t { return pending_commands_.size(); }

    /// Periodic tick: lock timeout (with its unsolicited LOCK_ENTITY release
    /// notification), pending acquire timeout, the IN_PROGRESS repeats of
    /// deferred commands and the deadline of those nobody completed, and under
    /// milan13 the controller monitor.
    void tick(sm::TimePoint now);

    /// Get the current configuration index
    [[nodiscard]] auto current_configuration() const noexcept -> uint16_t { return current_configuration_; }

    /// Set the current configuration index
    /// @param config The configuration index to set as current
    void set_current_configuration(uint16_t config) noexcept { current_configuration_ = config; }

  private:
    // Command Handlers — each writes its response body into `out_buffer` and
    // returns status + bytes-written. `out_buffer` is guaranteed to be at
    // least MAX_AEM_RESPONSE_SIZE bytes by process_packet().

    /// Handle READ_DESCRIPTOR command.
    [[nodiscard]] auto handle_read_descriptor(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle GET_NAME command (IEEE 1722.1 Clause 7.4.18). Parses the
    /// descriptor_type/index/name_index/config header and routes through
    /// AemEntityModel::get_name_for_wire -> the handler's symbol-keyed
    /// on_get_name. When the handler serves no name: NO_SUCH_DESCRIPTOR if
    /// the entity has no such descriptor (Table 7-141), else NOT_IMPLEMENTED.
    [[nodiscard]] auto handle_get_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle SET_NAME command (IEEE 1722.1 Clause 7.4.17). Routes through
    /// AemEntityModel::apply_set_name -> the handler's symbol-keyed
    /// on_set_name, echoing the command as the response.
    [[nodiscard]] auto handle_set_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// AVDECC exclusive-access gate (IEEE 1722.1 7.4). Returns the AEM status a
    /// mutating command must be rejected with when the entity is acquired or locked
    /// by a DIFFERENT controller (ENTITY_ACQUIRED / ENTITY_LOCKED), or nullopt when
    /// the sender owns the entity (or it is free) and the command may proceed.
    [[nodiscard]] auto check_exclusive_access(AemDu const& header) const noexcept -> std::optional<uint8_t>;

    /// check_exclusive_access() for a command from @p controller_entity_id, AEM or MVU.
    [[nodiscard]] auto exclusive_access_status(Eui64 const& controller_entity_id) const noexcept -> std::optional<uint8_t>;

    /// Build a rejection response: the given status, echoing the command payload so
    /// the controller can match the response. Applies no mutation.
    [[nodiscard]] static auto reject_command(uint8_t status, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Copy the command body into @p out_buffer as the response body; the bytes
    /// copied, 0 when it does not fit.
    [[nodiscard]] static auto echo_command(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> size_t;

    /// Handle GET_STREAM_FORMAT — returns the STREAM descriptor's current_format.
    [[nodiscard]] auto handle_get_stream_format(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle GET_SAMPLING_RATE — returns the AUDIO_UNIT descriptor's current_sampling_rate.
    [[nodiscard]] auto handle_get_sampling_rate(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle ENTITY_AVAILABLE command (no body).
    [[nodiscard]] static auto handle_entity_available(AemDu const& /*header*/) -> AemCommandResponse
    {
        return {.status = AEM_STATUS_SUCCESS, .size = 0};
    }

    /// Handle ACQUIRE_ENTITY (7.4.1). Only the ENTITY descriptor can be
    /// acquired here: any other descriptor type answers NOT_SUPPORTED.
    [[nodiscard]] auto handle_acquire_entity(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// The acquisition's RELEASE: SUCCESS for the owner, ENTITY_ACQUIRED for
    /// anyone else. An owner letting go while a CONTROLLER_AVAILABLE handshake
    /// is out hands the entity to the contender waiting on it.
    [[nodiscard]] auto release_acquisition(AemDu const& header) -> uint8_t;

    /// Take, keep or contest the acquisition: SUCCESS, IN_PROGRESS (a
    /// CONTROLLER_AVAILABLE handshake with the owner has begun, or is still
    /// out for this same contender) or ENTITY_ACQUIRED (the owner holds it,
    /// persistently or while a handshake is out for another contender).
    [[nodiscard]] auto take_acquisition(
        AemDu const& header, AemAcquireEntityPayload const& cmd, std::span<uint8_t const> command_data) -> uint8_t;

    /// Park the requester and ask the current owner whether it is still there.
    void begin_controller_available_handshake(AemDu const& header, uint32_t flags, std::span<uint8_t const> command_data);

    /// The owner is gone or has let go: the parked contender takes the entity
    /// and is told SUCCESS as its owner; the handshake is over.
    void grant_pending_acquire();

    /// Send the parked contender its ACQUIRE_ENTITY answer, @p status naming
    /// the owner now in place, and on SUCCESS the unsolicited notification.
    void answer_pending_acquire(uint8_t status);

    /// The ACQUIRE_ENTITY response for @p status: nothing yet for IN_PROGRESS,
    /// else the flags (zero after a successful release) and the owner.
    [[nodiscard]] auto acquire_outcome(
        uint8_t status, AemAcquireEntityPayload const& cmd, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    [[nodiscard]] static auto build_acquire_response(AcquireLockResponseParams const& params, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle LOCK_ENTITY (7.4.2). Only the ENTITY descriptor can be locked
    /// here (NOT_SUPPORTED otherwise), and while the entity is acquired only
    /// its acquirer may lock or unlock it (7.4.2.2: ENTITY_ACQUIRED otherwise).
    [[nodiscard]] auto handle_lock_entity(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Why a LOCK_ENTITY command is refused before the lock itself is consulted,
    /// or nullopt when it may proceed.
    [[nodiscard]] auto lock_refusal(AemDu const& header, AemLockEntityPayload const& cmd) const noexcept -> std::optional<uint8_t>;

    /// Take or refresh the lock: SUCCESS, or ENTITY_LOCKED when another controller holds it.
    [[nodiscard]] auto apply_lock(AemDu const& header) noexcept -> uint8_t;

    /// Release the lock: SUCCESS for the holder or a free entity, ENTITY_LOCKED otherwise.
    [[nodiscard]] auto apply_unlock(AemDu const& header) noexcept -> uint8_t;

    [[nodiscard]] static auto build_lock_response(AcquireLockResponseParams const& params, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle GET_CONFIGURATION command.
    [[nodiscard]] auto handle_get_configuration(AemDu const& header, std::span<uint8_t> out_buffer) const -> AemCommandResponse;

    /// Handle SET_CONFIGURATION command (kit phase 5c): validate against
    /// the ENTITY descriptor's configurations_count; an actual switch runs
    /// the on_configuration_changed veto/apply hook (refused NOT_SUPPORTED
    /// when no hook is registered); a SET to the current configuration is
    /// an idempotent SUCCESS. The dispatch answers a failure with the
    /// configuration in force (7.4.7.1).
    [[nodiscard]] auto handle_set_configuration(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// SET_CONFIGURATION, answered as refuse_unless_success() answers it; a switch forgets what
    /// belonged to the old configuration (forget_configuration_state()).
    [[nodiscard]] auto answer_set_configuration(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Forget what controllers set on the old configuration's descriptors: the media clock
    /// reference settings of its CLOCK_DOMAINs (Milan v1.3 Section 7.6), the dynamic audio mappings
    /// of its stream ports.
    void forget_configuration_state();

    /// Handle CONTROLLER_AVAILABLE command (always SUCCESS, no body).
    [[nodiscard]] static auto handle_controller_available(AemDu const& /*header*/) -> AemCommandResponse
    {
        return {.status = AEM_STATUS_SUCCESS, .size = 0};
    }

    /// Handle a SET descriptor-value command (SET_CONTROL, and the wider family).
    /// Routes through AemEntityModel::apply_set_descriptor_value -> the handler's
    /// symbol-keyed on_set_descriptor_value, echoing the command as the response.
    [[nodiscard]] auto handle_set_descriptor_value(
        uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle a GET descriptor-value command (GET_CONTROL, and the wider family).
    /// Routes through AemEntityModel::get_descriptor_value_for_wire -> the handler's
    /// symbol-keyed on_get_descriptor_value. When the handler serves no value:
    /// NO_SUCH_DESCRIPTOR if the entity has no such descriptor (Table 7-141),
    /// else NOT_IMPLEMENTED.
    [[nodiscard]] auto handle_get_descriptor_value(
        uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// The status for a GET the handler served nothing for: NO_SUCH_DESCRIPTOR
    /// when the entity has no descriptor at @p ref, else NOT_IMPLEMENTED, the
    /// entity having the descriptor but no value to give for it.
    [[nodiscard]] auto unserved_get_status(DescriptorRef const& ref) const -> uint8_t;
    /// The descriptor a GET descriptor-value body names (descriptor_type(2) +
    /// descriptor_index(2), in the current configuration), or nullopt for a
    /// body too short to name one.
    [[nodiscard]] auto descriptor_ref_of(std::span<uint8_t const> command_data) const -> std::optional<DescriptorRef>;
    /// The (descriptor, name_index) a GET_NAME body names (Clause 7.4.18.1),
    /// or nullopt for a body too short to name one.
    [[nodiscard]] static auto name_ref_of(std::span<uint8_t const> command_data) -> std::optional<NameRef>;

    /// Handle GET_COUNTERS command (IEEE 1722.1 Clause 7.4.42): read the target
    /// descriptor's counters from the get_counters callback and emit an
    /// AemCountersPayload response (descriptor_type/index + counters_valid + 32
    /// counter values). NOT_IMPLEMENTED if no callback; NO_SUCH_DESCRIPTOR if the
    /// descriptor has no counters.
    [[nodiscard]] auto handle_get_counters(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle GET_STREAM_INFO command (IEEE 1722.1 Clause 7.4.16): read the target
    /// stream's parameters from the get_stream_info callback and emit an
    /// AemStreamInfoPayload response. NOT_IMPLEMENTED if no callback;
    /// NO_SUCH_DESCRIPTOR if the descriptor is not one of this entity's streams.
    [[nodiscard]] auto handle_get_stream_info(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;
    /// GET_STREAM_INFO of the stream @p cmd names into @p out_buffer: NOT_IMPLEMENTED without the
    /// get_stream_info callback, NO_SUCH_DESCRIPTOR when it declines the stream.
    [[nodiscard]] auto answer_stream_info(AemGetStreamInfoCommandPayload const& cmd, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;
    /// @p resp at the head of @p out_buffer in stream_info_length() octets, zero past its 48;
    /// NOT_SUPPORTED when @p out_buffer is shorter.
    [[nodiscard]] auto store_stream_info(AemStreamInfoPayload const& resp, std::span<uint8_t> out_buffer) const
        -> AemCommandResponse;

    /// Handle GET_AVB_INFO (7.4.40) through the get_avb_info callback: the
    /// AemAvbInfoPayload followed by the MSRP mappings.
    [[nodiscard]] auto handle_get_avb_info(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle GET_AS_PATH (7.4.41) through the get_as_path callback: descriptor_index,
    /// count, then the path's clock identities.
    [[nodiscard]] auto handle_get_as_path(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle REBOOT (7.4.43) through the on_reboot callback; echoes the
    /// descriptor reference.
    [[nodiscard]] auto handle_reboot(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle SET_MAX_TRANSIT_TIME (7.4.76) through the presentation offset hooks: a STREAM_OUTPUT's
    /// offset, refused BAD_ARGUMENTS past MILAN_PRESENTATION_OFFSET_MAX_NS; answered, deferred and
    /// notified as a value SET is (handle_set_descriptor_value), a failure with the time in force.
    [[nodiscard]] auto handle_set_max_transit_time(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle GET_MAX_TRANSIT_TIME (7.4.77): a STREAM_OUTPUT's presentation time offset.
    [[nodiscard]] auto handle_get_max_transit_time(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// GET_MAX_TRANSIT_TIME @p cmd's answer, @p command_data echoed when it is refused.
    [[nodiscard]] auto answer_max_transit_time(
        AemGetMaxTransitTimeCommandPayload const& cmd, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// The presentation offset of MAX_TRANSIT_TIME's target in @p current, and SUCCESS; else
    /// NOT_IMPLEMENTED without the hooks, NOT_SUPPORTED for a STREAM_INPUT (the time is a
    /// talker's), NO_SUCH_DESCRIPTOR for any other descriptor or an index the hook declines.
    [[nodiscard]] auto max_transit_time_target(uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& current) const
        -> uint8_t;

    /// SUCCESS when SET_MAX_TRANSIT_TIME @p cmd names a talker's offset within
    /// MILAN_PRESENTATION_OFFSET_MAX_NS; else the status it is refused with.
    [[nodiscard]] auto max_transit_time_refusal(AemSetMaxTransitTimePayload const& cmd) const -> uint8_t;

    /// Put SET_MAX_TRANSIT_TIME @p cmd, whose body is @p body, in force through the set hook:
    /// answered and notified on SUCCESS, deferred on IN_PROGRESS, else refused with the time in force.
    [[nodiscard]] auto set_max_transit_time(
        AemSetMaxTransitTimePayload const& cmd, std::span<uint8_t const> body, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// A stream port's model, for the mapping commands.
    struct PortModel
    {
        uint16_t port_type{0};  ///< STREAM_PORT_INPUT or STREAM_PORT_OUTPUT
        uint16_t base_cluster{0};
        uint16_t number_of_clusters{0};
        uint16_t number_of_maps{0};
        uint16_t port_flags{0};
        uint32_t total_channels{0};  ///< over its clusters
        /// Each cluster's channel_count, read once (at most this many clusters).
        sg14::inplace_vector<uint16_t, 512> cluster_channel_counts{};
        /// An output port's: each STREAM_OUTPUT's channels, as its GET_AUDIO_MAP partitions count them.
        sg14::inplace_vector<uint16_t, MAX_PARTITIONED_STREAMS> stream_channel_counts{};
    };

    /// GET_AUDIO_MAP (7.4.44) on a port with dynamic mappings: partition map_index of them.
    /// A port with AUDIO_MAP descriptors is NOT_SUPPORTED.
    [[nodiscard]] auto handle_get_audio_map(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;
    /// The mappings a GET_AUDIO_MAP partition holds: MILAN_AUDIO_MAP_MAPPINGS under milan13, else AUDIO_MAP_MAPPINGS.
    [[nodiscard]] auto audio_map_partition() const noexcept -> size_t;
    /// GET_AUDIO_MAP's response for @p cmd on dynamic @p port.
    [[nodiscard]] auto audio_map_response(
        AemGetAudioMapCommandPayload const& cmd, PortModel const& port, std::span<uint8_t> out_buffer) const -> AemCommandResponse;
    /// Store the mappings of partition @p map_index of @p port (index @p port_index) in @p entries,
    /// as many as fit; their count.
    [[nodiscard]] auto store_audio_map_entries(
        PortModel const& port, uint16_t port_index, size_t map_index, std::span<uint8_t> entries) const -> uint16_t;
    /// Read stream port (@p port_type, @p port_index)'s model into @p model; false when there is none.
    [[nodiscard]] auto port_model(uint16_t port_type, uint16_t port_index, PortModel& model) const -> bool;
    /// Read the channel_count of each of @p model's clusters.
    void read_clusters(PortModel& model) const;
    /// Read each STREAM_OUTPUT's channels into an output @p model: the most any format it lists has,
    /// so its GET_AUDIO_MAP partition stays fixed for the configuration (Milan v1.3 Section 5.4.2.26).
    void read_output_streams(PortModel& model) const;
    /// The channels of cluster @p cluster_offset of @p port (0: none).
    [[nodiscard]] static auto cluster_channels(PortModel const& port, uint16_t cluster_offset) noexcept -> uint16_t;
    /// The position of @p mapping's cluster channel among @p port's (its clusters' channels in
    /// order), or nullopt when it names none.
    [[nodiscard]] static auto cluster_ordinal(PortModel const& port, DynamicAudioMapping const& mapping) noexcept
        -> std::optional<uint32_t>;
    /// The position of @p mapping's stream channel among output @p port's partitioned streams'.
    [[nodiscard]] static auto stream_ordinal(PortModel const& port, DynamicAudioMapping const& mapping) noexcept
        -> std::optional<uint32_t>;
    /// The position by which GET_AUDIO_MAP partitions @p mapping: an input's cluster channel,
    /// an output's stream channel (Milan v1.3 Section 5.4.2.26).
    [[nodiscard]] static auto partition_ordinal(PortModel const& port, DynamicAudioMapping const& mapping) noexcept
        -> std::optional<uint32_t>;
    /// The channels GET_AUDIO_MAP partitions on @p port: its clusters', or for an output its streams'.
    [[nodiscard]] static auto partitioned_channels(PortModel const& port) noexcept -> uint32_t;
    /// Port (@p port_type, @p port_index)'s model in @p port when its mappings are dynamic; else
    /// NOT_SUPPORTED (not a stream port, or AUDIO_MAP descriptors) or NO_SUCH_DESCRIPTOR.
    [[nodiscard]] auto dynamic_port(uint16_t port_type, uint16_t port_index, PortModel& port) const -> uint8_t;
    /// ADD_AUDIO_MAPPINGS (@p add) or REMOVE_AUDIO_MAPPINGS (7.4.45, 7.4.46) on a dynamic stream port.
    [[nodiscard]] auto handle_audio_mappings(bool add, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;
    /// ADD (@p add) or REMOVE_AUDIO_MAPPINGS @p cmd, its body @p command_data, on mappable @p port.
    [[nodiscard]] auto change_audio_mappings(
        bool add,
        AemAudioMappingsCommandHeader const& cmd,
        PortModel const& port,
        std::span<uint8_t const> command_data,
        std::span<uint8_t> out_buffer) -> AemCommandResponse;
    /// The set a controller's ADD (@p add) or REMOVE of @p listed leaves @p port (index @p port_index)
    /// at, in the store's next() (cluster order), or the status to refuse with: BAD_ARGUMENTS for a
    /// REMOVE of a mapping not held (7.4.46.1).
    [[nodiscard]] auto next_mappings(
        bool add, uint16_t port_index, PortModel const& port, std::span<DynamicAudioMapping const> listed) -> uint8_t;
    /// Put the store's next() in force for a controller's ADD (@p add) or REMOVE @p cmd on @p port_type
    /// and answer it (the command echoed), notifying the other controllers when it changed the port.
    [[nodiscard]] auto commit_mappings_command(
        bool add,
        uint16_t port_type,
        AemAudioMappingsCommandHeader const& cmd,
        std::span<uint8_t const> command_data,
        std::span<uint8_t> out_buffer) -> AemCommandResponse;
    /// SUCCESS when @p mapping names a cluster channel of the port and a stream of its direction and,
    /// with @p check_channel, a channel of its current format at the unit's rate; else BAD_ARGUMENTS.
    [[nodiscard]] auto check_mapping(
        uint16_t port_index, PortModel const& port, DynamicAudioMapping const& mapping, bool check_channel) const -> uint8_t;
    /// SUCCESS when @p stream_channel is a channel of @p stream_format and, on a port that does not
    /// convert rates, the format is at its audio unit's rate; else BAD_ARGUMENTS.
    [[nodiscard]] auto check_stream_channel(
        uint16_t port_index, PortModel const& port, uint64_t stream_format, uint16_t stream_channel) const -> uint8_t;
    /// Whether the audio unit holding stream port (@p port_type, @p port_index) runs at base rate
    /// @p rate (true: no such unit).
    [[nodiscard]] auto unit_rate_is(uint16_t port_type, uint16_t port_index, uint32_t rate) const -> bool;
    /// dynamic_port() for a port a controller's ADD or REMOVE may change (with the store and the
    /// data plane's hook); else the status they refuse with.
    [[nodiscard]] auto mappable_port(uint16_t port_type, uint16_t port_index, PortModel& port) const -> uint8_t;
    /// SET_STREAM_FORMAT @p cmd's refusal: a mapping of its stream whose channel or rate it leaves.
    [[nodiscard]] auto format_change_refusal(AemStreamFormatPayload const& cmd) const -> std::optional<uint8_t>;
    /// SET_SAMPLING_RATE @p cmd's refusal: a mapping on a port of its unit, not converting rates,
    /// whose stream runs at another rate.
    [[nodiscard]] auto rate_change_refusal(AemSamplingRatePayload const& cmd) const -> std::optional<uint8_t>;
    /// Whether stream port (@p port_type, @p port_index) converts rates (Table 7-15).
    [[nodiscard]] auto port_converts(uint16_t port_type, uint16_t port_index) const -> bool;
    /// The rate of the current format of the stream @p stream_index of @p port_type's direction (0: none).
    [[nodiscard]] auto stream_rate(uint16_t port_type, uint16_t stream_index) const -> uint32_t;
    /// dynamic_port() when the entity has a store, else NOT_IMPLEMENTED.
    [[nodiscard]] auto stored_port(uint16_t port_type, uint16_t port_index, PortModel& port) const -> uint8_t;
    /// Append @p mappings, checked as an ADD's, to the set in the store's next(): BAD_ARGUMENTS for an
    /// input's cluster channel, or an output's stream channel, mapped twice; NO_RESOURCES past capacity.
    [[nodiscard]] auto add_mappings(uint16_t port_index, PortModel const& port, std::span<DynamicAudioMapping const> mappings)
        -> uint8_t;
    /// add_mappings() for one mapping.
    [[nodiscard]] auto add_mapping(uint16_t port_index, PortModel const& port, DynamicAudioMapping const& mapping) -> uint8_t;
    /// @p current without @p listed (each checked against the model, one not held ignored) in the
    /// store's next(), or the status to refuse with.
    [[nodiscard]] auto remove_mappings(
        uint16_t port_index,
        PortModel const& port,
        std::span<DynamicAudioMapping const> current,
        std::span<DynamicAudioMapping const> listed) -> uint8_t;
    /// Whether the store's next() fits as the port's beside the other ports' and, with @p apply, the
    /// hook puts it in force (NOT_IMPLEMENTED without one): SUCCESS, or the status to refuse with.
    [[nodiscard]] auto admit_mappings(uint16_t port_type, uint16_t port_index, bool apply) -> uint8_t;
    /// admit_mappings(), then keep the store's next() as the port's.
    [[nodiscard]] auto admit_and_keep(uint16_t port_type, uint16_t port_index, bool apply) -> uint8_t;
    /// The set the entity's REMOVE of @p remove, then ADD of @p add, leaves @p port (index
    /// @p port_index) at, in the store's next() (cluster order), or the status to refuse with.
    [[nodiscard]] auto local_mappings(
        uint16_t port_index,
        PortModel const& port,
        std::span<DynamicAudioMapping const> add,
        std::span<DynamicAudioMapping const> remove) -> uint8_t;
    /// Make the store's next() the entity's own change of port (@p port_type, @p port_index): put in
    /// force through the hook, told to every registered controller, kept. SUCCESS, with nothing done,
    /// when it changes nothing.
    [[nodiscard]] auto keep_own_mappings(uint16_t port_type, uint16_t port_index) -> uint8_t;
    /// Tell every registered controller how the store's next() differs from @p current, the port's
    /// mappings in the store, then keep it there.
    void tell_and_keep(uint16_t port_type, uint16_t port_index, std::span<DynamicAudioMapping const> current);
    /// Unsolicited ADD (@p add) or REMOVE_AUDIO_MAPPINGS on (@p port_type, @p port_index) of the
    /// mappings in @p of but not in @p but (both in cluster order), in AECPDUs of at most 63.
    void notify_mappings(
        bool add,
        uint16_t port_type,
        uint16_t port_index,
        std::span<DynamicAudioMapping const> of,
        std::span<DynamicAudioMapping const> but);
    /// One unsolicited ADD (@p add) or REMOVE_AUDIO_MAPPINGS of the @p count mappings in @p body
    /// after its header, which this fills in.
    void emit_mappings_pdu(bool add, uint16_t port_type, uint16_t port_index, std::span<uint8_t> body, size_t count);
    /// Descriptor (@p type, @p index) of the current configuration, nullopt when there is none.
    template <typename T>
    [[nodiscard]] auto read_model_descriptor(uint16_t type, uint16_t index) const -> std::optional<T>;

    /// Handle SET_MEMORY_OBJECT_LENGTH / GET_MEMORY_OBJECT_LENGTH (7.4.72 /
    /// 7.4.73) through the handler's on_set/get_memory_object_length hooks.
    [[nodiscard]] auto handle_set_memory_object_length(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;
    [[nodiscard]] auto handle_get_memory_object_length(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle GET_DYNAMIC_INFO (7.4.76, list per Cor 1-2025): every dynamic_info
    /// entry must name a fixed-size GET (else BAD_ARGUMENTS for the whole
    /// command); each is then dispatched as an independent command and its
    /// status and response (or the echoed request on failure) packed back,
    /// dropping entries that would overflow the AECPDU.
    [[nodiscard]] auto handle_get_dynamic_info(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Defer a SET whose hook answered IN_PROGRESS: keep (header, source MAC,
    /// echoed body) for the periodic IN_PROGRESS repeats and the final response.
    [[nodiscard]] auto defer_command(uint16_t command_type, std::span<uint8_t const> echoed_body) -> AemCommandResponse;

    /// A SET's final answer: SUCCESS echoes the command (@p echoed) and notifies
    /// subscribers; anything else answers the value in force.
    [[nodiscard]] auto settle_set(
        uint16_t command_type,
        uint8_t status,
        std::span<uint8_t const> command_data,
        std::span<uint8_t const> echoed,
        std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Run a value SET through check_exclusive_access() first; a refusal still
    /// answers the value in force.
    [[nodiscard]] auto guarded_set(
        AemDu const& header, uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Under milan13: whether @p command_data asks for the value already in
    /// force, read back as GET reads it. Such a SET modifies nothing, so it is
    /// not applied and not notified (Milan v1.3 5.4.5.3). Always false under
    /// classic, where 7.5.2 notifies every SET answered SUCCESS.
    [[nodiscard]] auto set_leaves_value_as_is(uint16_t command_type, std::span<uint8_t const> command_data) -> bool;

    /// The SUCCESS answer (the command echoed) for a SET that leaves the value
    /// as it is, or nullopt when the SET has something to apply.
    [[nodiscard]] auto unchanged_set_answer(
        uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> std::optional<AemCommandResponse>;

    /// What a value SET is answered without its hook running: IN_PROGRESS
    /// again for the retransmit of a deferred command, NO_RESOURCES when no
    /// pending slot is left for it, mappings_refusal(), or
    /// unchanged_set_answer(); nullopt when the hook is to run.
    [[nodiscard]] auto answered_before_hook(
        uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> std::optional<AemCommandResponse>;

    /// A SET_NAME that took: the command echoed, and the subscribers notified.
    [[nodiscard]] auto applied_set_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Apply a value change through the handler's hook and notify subscribers
    /// on SUCCESS; the status. The body of apply_local_descriptor_value().
    [[nodiscard]] auto apply_and_notify(uint16_t command_type, std::span<uint8_t const> command_body) -> uint8_t;

    /// The response of a SET that did not take: @p status with failed_set_body().
    [[nodiscard]] auto refused_set(
        uint8_t status, uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// The body a failed SET answers with (7.4.9.1 and its siblings: "the old
    /// value if it fails"): the value in force as GET reads it, else the command
    /// echoed. Returns the bytes written into @p out_buffer.
    [[nodiscard]] auto failed_set_body(uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> size_t;

    /// The value in force for the descriptor @p command_data (a SET body)
    /// addresses, framed as the SET's GET twin frames it; 0 when there is no
    /// twin or nothing to read.
    [[nodiscard]] auto value_in_force(
        uint16_t set_command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> size_t;

    /// Dispatch @p get_command_type with @p request as if it had arrived, keeping
    /// the current command's header; the response body size on SUCCESS, else 0.
    [[nodiscard]] auto read_back(uint16_t get_command_type, std::span<uint8_t const> request, std::span<uint8_t> out_buffer)
        -> size_t;

    /// The GET that reads what a SET writes (SET_NAME -> GET_NAME, SET_CONTROL ->
    /// GET_CONTROL, ...), or nullopt for a SET without one.
    [[nodiscard]] static auto get_twin_of(uint16_t set_command_type) noexcept -> std::optional<uint16_t>;

    /// @p set_command's response @p r on SUCCESS, else refused_set()'s, for a SET the handler
    /// answers itself and never defers (SET_CONFIGURATION): an IN_PROGRESS from its hook, which
    /// no final response could follow, is answered ENTITY_MISBEHAVING.
    [[nodiscard]] auto refuse_unless_success(
        uint16_t set_command, AemCommandResponse r, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// How much of a SET body its GET twin asks with: the descriptor head, the
    /// name head for SET_NAME, the region header for SET_MATRIX.
    [[nodiscard]] static auto get_twin_request_size(uint16_t set_command_type) noexcept -> size_t;

    /// After a command handled with SUCCESS, send its response body as the
    /// unsolicited notification (IEEE 1722.1 7.5.2 list).
    void notify_on_success(uint16_t command_type, AemCommandResponse const& response, std::span<uint8_t const> out_buffer);

    /// Unsolicited LOCK_ENTITY (UNLOCK, locked_entity_id 0) when a lock expires.
    void emit_lock_released();

    /// DescriptorId for MEMORY_OBJECT @p descriptor_index in @p configuration_index.
    [[nodiscard]] auto memory_object_id(uint16_t configuration_index, uint16_t descriptor_index) const -> DescriptorId;

    /// Handle REGISTER_UNSOLICITED_NOTIFICATION command (no body).
    [[nodiscard]] auto handle_register_unsolicited(AemDu const& header) -> AemCommandResponse;

    /// Handle DEREGISTER_UNSOLICITED_NOTIFICATION command (no body).
    [[nodiscard]] auto handle_deregister_unsolicited(AemDu const& header) -> AemCommandResponse;

    /// Frame one unsolicited AEM response (U-bit set) for @p command_type / @p body,
    /// addressed to @p controller_id with @p sequence_id, and send it to @p dest_mac
    /// via the callback.
    void send_unsolicited_to(
        Eui48 const& dest_mac,
        Eui64 const& controller_id,
        uint16_t sequence_id,
        uint16_t command_type,
        std::span<uint8_t const> body);

    /// True if (@p command_type, @p body) is a SET_CONTROL targeting the configured
    /// IDENTIFY control — i.e. the change must also go to the IDENTIFY multicast.
    [[nodiscard]] auto targets_identify_control(uint16_t command_type, std::span<uint8_t const> body) const noexcept -> bool;

    /// Build and send AECP AEM response.
    /// @param dest_mac Destination MAC address for the response
    /// @param cmd_header The original command header to echo in the response
    /// @param status The AEM status code for the response
    /// @param response_body Bytes to place after the AemDu header (may be empty)
    [[nodiscard]] auto send_response(
        Eui48 const& dest_mac, AemDu const& cmd_header, uint8_t status, std::span<uint8_t const> response_body) const -> bool;

    /// A Milan vendor-unique command addressed to this entity: the MVU header
    /// and the command-specific data its control_data_length covers, when
    /// @p payload is one; nullopt for anything else (another vendor's protocol,
    /// a response, a truncated header, another target).
    struct MvuCommand
    {
        AecpMvuDu header{};
        std::span<uint8_t const> data{};
    };
    [[nodiscard]] static auto mvu_command_for(std::span<uint8_t const> payload, Eui64 const& our_entity_id) noexcept
        -> std::optional<MvuCommand>;

    /// An AEM response addressed to us as its controller arrived: CONTROLLER_AVAILABLE's
    /// answers the acquire handshake and the controller monitor; any other is dropped.
    void handle_aem_response(AemDu const& header);

    /// A valid AECP command from @p controller_entity_id arrived: its monitor timer starts
    /// again, and a CONTROLLER_AVAILABLE outstanding for it is forgotten (Milan v1.3 Section 5.4.5.4).
    void restart_controller_monitor(Eui64 const& controller_entity_id);

    /// A random monitor period in [CONTROLLER_MONITOR_MIN, CONTROLLER_MONITOR_MAX].
    [[nodiscard]] auto monitor_period() noexcept -> sm::Clock::duration;

    /// The controller monitor, from tick() under milan13.
    void tick_controller_monitor(sm::TimePoint now);

    /// Send CONTROLLER_AVAILABLE (IEEE 1722.1-2021 Clause 7.4.4, no payload) to @p controller_id
    /// at @p dest_mac with @p sequence_id, this entity its controller.
    void send_controller_available_to(Eui48 const& dest_mac, Eui64 const& controller_id, uint16_t sequence_id);

    /// An AEM command addressed to us arrived: dispatch it and send the response.
    [[nodiscard]] auto process_aem_command(
        Eui48 const& src_mac, AemDu const& header, std::span<uint8_t const> command_data, Eui64 const& our_entity_id) -> bool;

    /// handle_command() for a command off the wire from @p src_mac, the notifications it
    /// raises held for after its response.
    [[nodiscard]] auto dispatch_aem_command(
        Eui48 const& src_mac, AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Send the response to @p header, then the notifications its dispatch held.
    [[nodiscard]] auto answer_aem_command(
        Eui48 const& src_mac, AemDu const& header, AemCommandResponse const& response, std::span<uint8_t const> body) -> bool;

    /// Hold a notification raised while a command is dispatched, or spend one of the command's own
    /// type that finds no slot; false (send it now, after the side effects held) for a side effect
    /// with no slot free, or a body longer than MAX_AEM_RESPONSE_SIZE.
    [[nodiscard]] auto queue_notification(uint16_t command_type, std::span<uint8_t const> body) -> bool;

    /// Send nothing for a notification of the command's own type the queue cannot hold, but spend
    /// each recipient's sequence id as a failed send does (Milan v1.3 Section 5.4.5.1).
    void spend_notification(uint16_t command_type, std::span<uint8_t const> body);

    /// Put @p body, raised as @p command_type, in a slot make_room() found.
    void hold_notification(uint16_t command_type, std::span<uint8_t const> body);

    /// Whether a notification of @p command_type is the dispatched command's own.
    [[nodiscard]] auto is_commands_own(uint16_t command_type) const noexcept -> bool;

    /// Whether a notification of @p command_type has a slot, after sending the held side
    /// effects if need be; the last slot is kept for the command's own.
    [[nodiscard]] auto make_room(uint16_t command_type) -> bool;

    /// Send the held notifications other than the command's own, in the order raised.
    void release_side_effects();

    /// Send every held notification, after the command's response.
    void flush_notifications();

    /// Run what call_after_response() held, @p requester (the command's controller) left out of
    /// what it notifies.
    void run_after_response(Eui64 const& requester);

    /// A VENDOR_UNIQUE command arrived: answer it if it is Milan's and this is a Milan device.
    [[nodiscard]] auto process_mvu_packet(Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id)
        -> bool;

    /// Answer a Milan command addressed to this entity, its controller the requester that the
    /// notifications its dispatch raises leave out.
    [[nodiscard]] auto dispatch_mvu_command(Eui48 const& src_mac, MvuCommand const& command) -> bool;

    /// Answer an MVU command that is not a bind or unbind: handle_mvu_command(), then the
    /// response, then, when the command changed what it names, the same response to the other
    /// registered controllers (Milan v1.3 Section 5.4.5.3).
    [[nodiscard]] auto answer_mvu_command(Eui48 const& src_mac, MvuCommand const& command) -> bool;

    /// Dispatch one MVU command that is answered by the handler (Milan v1.3
    /// 5.4.4): GET_MILAN_INFO from milan_info_, SET and GET_SYSTEM_UNIQUE_ID from
    /// system_unique_id(), SET and GET_MEDIA_CLOCK_REFERENCE_INFO
    /// from the clock domains' settings, GET_STREAM_INPUT_INFO_EX from the attached
    /// stream inputs, every other command NOT_IMPLEMENTED with its data given back.
    /// Writes the response's command-specific data into @p out_buffer.
    [[nodiscard]] auto handle_mvu_command(MvuCommand const& command, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// GET_SYSTEM_UNIQUE_ID (5.4.4.3): the id in force; PAYLOAD_TOO_SHORT without its reserved octets.
    [[nodiscard]] auto handle_mvu_get_system_unique_id(MvuCommand const& command, std::span<uint8_t> out_buffer) const
        -> AemCommandResponse;

    /// SET_SYSTEM_UNIQUE_ID (5.4.4.2): the id and a well-formed UTF-8 name, from a controller the
    /// lock does not shut out (ENTITY_LOCKED, else BAD_ARGUMENTS). Answers the id in force.
    [[nodiscard]] auto handle_mvu_set_system_unique_id(MvuCommand const& command, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// The status SET_SYSTEM_UNIQUE_ID @p request from @p controller answers, before anything is
    /// applied: ENTITY_LOCKED, BAD_ARGUMENTS or SUCCESS.
    [[nodiscard]] auto system_unique_id_set_status(Eui64 const& controller, MvuSystemUniqueIdPayload const& request) const
        -> uint8_t;

    /// Keep the id and name of @p request; mvu_changed_ says whether they changed anything, and
    /// then the hook is told.
    void apply_system_unique_id(MvuSystemUniqueIdPayload const& request) noexcept;

    /// GET_MEDIA_CLOCK_REFERENCE_INFO (5.4.4.5): a CLOCK_DOMAIN the model lacks is
    /// NO_SUCH_DESCRIPTOR with the command given back.
    [[nodiscard]] auto handle_mvu_get_mcr_info(MvuCommand const& command, std::span<uint8_t> out_buffer) const
        -> AemCommandResponse;

    /// SET_MEDIA_CLOCK_REFERENCE_INFO (5.4.4.4): the fields its flags mark, a well-formed UTF-8
    /// name, from a controller the lock does not shut out. Answers the settings in force, the
    /// command given back for a CLOCK_DOMAIN the model lacks.
    [[nodiscard]] auto handle_mvu_set_mcr_info(MvuCommand const& command, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// The status SET_MEDIA_CLOCK_REFERENCE_INFO @p request from @p controller answers, before
    /// anything is applied: NO_SUCH_DESCRIPTOR, BAD_ARGUMENTS, ENTITY_LOCKED or SUCCESS.
    [[nodiscard]] auto mcr_set_status(Eui64 const& controller, MvuMediaClockReferenceInfoPayload const& request) const -> uint8_t;

    /// What SET_MEDIA_CLOCK_REFERENCE_INFO @p request is answered with: the settings in force, or the
    /// command given back for a CLOCK_DOMAIN the model lacks.
    [[nodiscard]] auto mcr_info_answer(MvuMediaClockReferenceInfoPayload const& request) const -> MvuMediaClockReferenceInfoPayload;

    /// Apply the fields of @p request its flags mark; mvu_changed_ says whether they changed anything.
    void apply_mcr_info(MvuMediaClockReferenceInfoPayload const& request);

    /// Whether the current configuration has CLOCK_DOMAIN @p index whose settings are kept.
    [[nodiscard]] auto has_clock_domain(uint16_t index) const -> bool;

    /// GET_MILAN_INFO (5.4.4.1).
    [[nodiscard]] auto handle_mvu_get_milan_info(MvuCommand const& command, std::span<uint8_t> out_buffer) const
        -> AemCommandResponse;

    /// GET_STREAM_INPUT_INFO_EX (5.4.4.8).
    [[nodiscard]] auto handle_mvu_get_stream_input_info_ex(MvuCommand const& command, std::span<uint8_t> out_buffer) const
        -> AemCommandResponse;

    /// The answer to a GET_STREAM_INPUT_INFO_EX that names @p asked.
    [[nodiscard]] auto mvu_stream_input_info_ex(MvuStreamInput const& asked, std::span<uint8_t> out_buffer) const
        -> AemCommandResponse;

    /// True for a BIND_STREAM or UNBIND_STREAM command that the attached
    /// stream inputs take.
    [[nodiscard]] auto is_mvu_binding_command(MvuCommand const& command) const noexcept -> bool;

    /// answer_mvu_binding_command(), and then the command's notification to
    /// the other registered controllers if it changed the binding of the
    /// stream input it names (5.4.5.3).
    auto answer_mvu_binding_and_notify(Eui48 const& src_mac, MvuCommand const& command) -> bool;

    /// The binding in force of the stream input the BIND_STREAM or
    /// UNBIND_STREAM @p command names; nullopt for one not bound, for a
    /// command too short to name one and with no binding callback attached.
    [[nodiscard]] auto mvu_binding_in_force(MvuCommand const& command) const -> std::optional<atdecc::MilanBinding>;

    /// BIND_STREAM and UNBIND_STREAM (5.4.4.6, 5.4.4.7): a command the
    /// handler can refuse by itself is answered here, any other goes to its
    /// sink as the ACMP command it equals, and the response is sent from
    /// inside that treatment, where the sink would send its ACMP response.
    [[nodiscard]] auto answer_mvu_binding_command(Eui48 const& src_mac, MvuCommand const& command) const -> bool;

    /// The second half of answer_mvu_binding_command(): @p as_acmp goes to
    /// its sink, and the response to @p header, carrying @p response_data,
    /// is sent when the sink answers.
    [[nodiscard]] auto give_mvu_binding_to_its_sink(
        Eui48 const& src_mac,
        AecpMvuDu const& header,
        atdecc::AcmpCommandResponse const& as_acmp,
        std::span<uint8_t const> response_data) const -> bool;

    /// The GET_MILAN_INFO response data for milan_info_ (which must be set).
    [[nodiscard]] auto milan_info_response() const noexcept -> MvuGetMilanInfoResponse;

    /// Send the MVU response to @p cmd_header: its header with @p status and
    /// @p response_body as the command-specific data, through send_response.
    [[nodiscard]] auto send_mvu_response(
        Eui48 const& dest_mac, AecpMvuDu const& cmd_header, uint8_t status, std::span<uint8_t const> response_body) const -> bool;

    /// Send @p header followed by @p body to @p dest_mac through send_response; false
    /// without the callback or for a body longer than MAX_AEM_RESPONSE_SIZE.
    [[nodiscard]] auto send_mvu_pdu(Eui48 const& dest_mac, AecpMvuDu const& header, std::span<uint8_t const> body) const -> bool;

    AemCommandHandlerCallbacks callbacks_;
    MvuStreamInputCallbacks mvu_stream_inputs_{};
    void const* mvu_stream_inputs_owner_{nullptr};  ///< see attach_mvu_stream_inputs

    // One of two code paths:
    //  (1) Legacy `EntityModel&` constructor: adapter_storage_ holds an
    //      EntityModelAdapter wrapping the supplied EntityModel, and
    //      handler_ points at it.
    //  (2) New `AemEntityHandler&` constructor: adapter_storage_ is
    //      empty, and handler_ points at the caller-supplied handler.
    //
    // `handle_read_descriptor` materializes a local `AemEntityModel{*handler_}`
    // on each call to do the dispatch — constructing the model per call
    // is free (two pointers plus an optional) and avoids a dangling
    // self-pointer if this object is moved. Phase 5 will delete the
    // EntityModelAdapter path entirely.
    std::optional<EntityModelAdapter> adapter_storage_;
    AemEntityHandler* handler_{nullptr};

    // Emit 2013/2016-length descriptors in READ_DESCRIPTOR responses.
    bool legacy_2016_ = false;

    // SET_CONTROL of the IDENTIFY control ignores acquire and lock (an option;
    // the standards refuse it like any other SET_CONTROL).
    bool identify_bypasses_exclusive_access_ = false;

    // Which specification settles the points where 1722.1 and Milan differ.
    Profile profile_ = Profile::classic;

    // The GET_MILAN_INFO answer; nullopt for an entity that is not a Milan device.
    std::optional<MilanInfo> milan_info_{};

    // One CLOCK_DOMAIN's media clock reference settings (Milan v1.3 Section 7.6): the user
    // priority a controller set (none: the default priority's) and the domain name.
    struct McrInfo
    {
        std::optional<uint8_t> user_mcr_prio{};
        aem::AtdeccString domain_name{atdecc::MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME};
    };
    std::array<McrInfo, MAX_CLOCK_DOMAINS> mcr_info_{};
    std::array<uint8_t, MAX_CLOCK_DOMAINS> default_mcr_prio_{[] {
        std::array<uint8_t, MAX_CLOCK_DOMAINS> prio{};
        prio.fill(atdecc::MVU_MCR_PRIO_DEFAULT);
        return prio;
    }()};
    // The system the entity belongs to (SET_SYSTEM_UNIQUE_ID), entity-wide, not per configuration.
    ieee::octlet_t system_unique_id_{0};
    aem::AtdeccString system_name_{};
    // A command off the wire is being dispatched (dispatch_aem_command); a read_back() does not set it.
    bool dispatching_{false};
    // The MVU command answer_mvu_command() dispatches changed what it names: its response is notified.
    bool mvu_changed_{false};

    // Acquisition state. The acquirer's MAC is kept because IEEE 1722.1 7.5.2
    // implicitly registers the acquiring controller for unsolicited notifications.
    bool acquired_ = false;
    bool acquired_persistent_ = false;
    Eui64 acquiring_controller_;
    Eui48 acquiring_controller_mac_{};

    // Lock state
    bool locked_ = false;
    Eui64 locking_controller_;
    sm::TimePoint lock_expiry_time_{};

    // Current event time (set_event_time(), or a later tick())
    sm::TimePoint event_time_{};

    // Source MAC of the request currently being dispatched. Stashed by
    // process_packet() before calling handle_command() so per-command
    // handlers (specifically handle_acquire_entity) can retain it in
    // pending_acquire_ for later out-of-band response sending.
    Eui48 current_src_mac_{};

    // Header of the command currently being dispatched (set by handle_command),
    // retained by defer_command() for a deferred command's later responses.
    AemDu current_header_{};

    // Deferred commands awaiting complete_pending_command().
    struct PendingCommand
    {
        AemDu header{};
        Eui48 mac{};
        std::array<uint8_t, MAX_PENDING_BODY> body{};
        size_t body_size{0};
        sm::TimePoint next_in_progress{};
        /// When tick() gives up on it (PENDING_COMMAND_DEADLINE_MS after deferral).
        sm::TimePoint deadline{};
    };
    SlotTable<PendingCommand, MAX_PENDING_COMMANDS> pending_commands_{};

    /// The deferred command the dispatch in progress repeats, if it is one:
    /// same controller, sequence_id and command as a slot already holds (a
    /// controller's retransmit, 9.2.2.3.1.2), else nullptr.
    [[nodiscard]] auto repeated_pending(uint16_t command_type) const noexcept -> PendingCommand const*;

    /// Answer every deferred command past its deadline with
    /// PENDING_COMMAND_EXPIRED_STATUS and the value in force, freeing its slot.
    void expire_pending_commands(sm::TimePoint now);

    /// Send a completed deferred command's final response (and its unsolicited
    /// notification on SUCCESS); see complete_pending_command() for the body.
    void answer_completed(PendingCommand const& done, uint8_t status, std::span<uint8_t const> response_body);

    /// The body of that final response: what the application supplied, else
    /// the command echoed on SUCCESS, else the value in force read into @p in_force.
    [[nodiscard]] auto completion_body(
        PendingCommand const& done, uint8_t status, std::span<uint8_t const> response_body, std::span<uint8_t> in_force)
        -> std::span<uint8_t const>;

    /// emit_unsolicited() for a deferred command's answer, with @p requester
    /// (its controller) left out as handle_command() would (7.5.2).
    void emit_unsolicited_for(Eui64 const& requester, uint16_t command_type, std::span<uint8_t const> body);

    /// The one switch over the command codes behind handle_command().
    [[nodiscard]] auto dispatch_command(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// The controller whose command is being answered, for the unsolicited
    /// fan-out to leave out (7.5.2). Set for the span of one dispatch
    /// (handle_command, answer_completed) and zero between: a notification
    /// the entity originates itself has no requester to leave out.
    Eui64 requester_{};

    // Notifications a command's dispatch raises, held until its response is sent; the last
    // slot is kept for the command's own (of the command's type), each with its requester_.
    // A body is held up to any length the entity frames, Milan's larger AECPDUs included.
    struct QueuedNotification
    {
        uint16_t command_type{0};
        Eui64 requester{};
        std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> body{};
        size_t body_size{0};
    };
    /// Send held notification @p n, leaving out the requester_ it was raised with.
    void emit_held(QueuedNotification const& n);
    statusbar::sg14::inplace_vector<QueuedNotification, MAX_QUEUED_NOTIFICATIONS> queued_notifications_{};
    // What the dispatched command's hooks left for after its response (call_after_response).
    statusbar::sg14::inplace_vector<AfterResponseFn, MAX_AFTER_RESPONSE> after_response_{};

    // Pending acquire state for CONTROLLER_AVAILABLE handshake.
    // When active, the requester is waiting on a final response that will
    // be sent from controller_available_response_received() (DENY) or
    // controller_available_timed_out() (GRANT).
    struct PendingAcquire
    {
        AemDu requester_header{};
        Eui48 requester_mac{};
        uint32_t requester_flags{0};
        std::array<uint8_t, 16> command_data_copy{};
        sm::TimePoint timeout{};
        bool active{false};
    };
    PendingAcquire pending_acquire_{};

    // Current configuration
    uint16_t current_configuration_ = 0;
    statusbar::sg14::inplace_function<uint8_t(uint16_t), 64> on_configuration_changed_{};
    SystemUniqueIdChangedFn on_system_unique_id_changed_{};

    // Unsolicited notification registrations.
    // Slot occupancy is tracked by the enclosing SlotTable (its size() is
    // the source of truth), so there is no longer a `bool active` field.
    struct UnsolicitedRegistration
    {
        Eui64 controller_entity_id{};
        Eui48 controller_mac{};       ///< Source MAC of the REGISTER command, for unicast notifications
        uint16_t sequence_id{0};      ///< of its next AEM notification (Milan v1.3 Section 5.3.4.2)
        uint16_t mvu_sequence_id{0};  ///< of its next MVU notification
        /// When the monitor next sends CONTROLLER_AVAILABLE or, with one outstanding, when that times out.
        sm::TimePoint monitor_deadline{};
        uint8_t available_tries{0};         ///< CONTROLLER_AVAILABLE sent and unanswered: 0, 1, or 2 with the retry
        uint16_t available_sequence_id{0};  ///< of the one outstanding
    };
    SlotTable<UnsolicitedRegistration, MAX_UNSOLICITED_REGISTRATIONS> unsolicited_registrations_{};

    /// Whether @p reg stays registered at @p now: its timer has not run out, or CONTROLLER_AVAILABLE
    /// or its one retry (IEEE 1722.1-2021 Clause 9.3.6) is sent to it now.
    [[nodiscard]] auto keep_monitoring(UnsolicitedRegistration& reg, sm::TimePoint now) -> bool;

    /// Send @p reg CONTROLLER_AVAILABLE, or its retry with the same sequence id, at @p now.
    void ask_controller_available(UnsolicitedRegistration& reg, sm::TimePoint now);

    // Sequence ids of the commands this entity sends as a controller (CONTROLLER_AVAILABLE).
    uint16_t command_sequence_id_{0};
    // The monitor period's xorshift32 state.
    uint32_t monitor_random_{0x2545F491U};

    // Our own entity_id, needed as the target_entity_id of unsolicited
    // notifications. Stashed by process_packet (always current) and settable
    // directly via set_entity_id() for notifications emitted before any command
    // has been received (e.g. an IDENTIFY button press at startup).
    Eui64 our_entity_id_{};

    // Sequence_id of the unsolicited notifications that go to no registration: an
    // acquirer that never registered, the IDENTIFY multicast (IEEE 1722.1 9.2.1.2.4).
    uint16_t unsolicited_sequence_id_{0};

    // The CONTROL descriptor index of the entity's IDENTIFY control, if it has one.
    // When valid, a SET_CONTROL on this index also notifies the IDENTIFY multicast.
    uint16_t identify_control_index_{0};
    bool identify_control_index_valid_{false};

    // The stream ports' dynamic audio mappings, in the entity's store (set_dynamic_audio_maps).
    DynamicAudioMaps* audio_maps_{nullptr};
};

}  // namespace statusbar::nanoavb
