#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Shared model types for the ATDECC controller — consumed by both the
/// network-side facade (ControllerSimple) and any presentation layer
/// (ControllerTuiApp, Lua scripts, GUIs, autonomous services).
///
/// These types carry no UI state. They are the command/event/data protocol
/// between the business logic and whatever drives it.

#include "statusbar/atdecc/atdecc_aecp_aem_controller.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_vector.h"

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace statusbar::atdecc_tools {

//
// Command types (sent from a driver to the controller facade)
//

/// Describes a stream connection request (talker + listener) or AEM command target.
struct StreamRequest
{
    ieee::Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};
    ieee::Eui64 listener_entity_id{};
    uint16_t listener_unique_id{0};
    uint16_t desc_type{0};           ///< Descriptor type for AEM commands
    uint16_t desc_index{0};          ///< Descriptor index for AEM commands
    uint64_t stream_format{0};       ///< Stream format for SET_STREAM_FORMAT
    uint16_t clock_source_index{0};  ///< Clock source index for SET_CLOCK_SOURCE
    int8_t identify_state{-1};       ///< IdentifyEntity: -1 = toggle, 0 = off, 1 = on
    uint16_t signal_type{0};         ///< SetSignalSelector: source signal_type
    uint16_t signal_index{0};        ///< SetSignalSelector: source signal_index
    uint16_t signal_output{0};       ///< SetSignalSelector: source signal_output

    /// SetControl: the raw value payload (the bytes after the 4-byte
    /// descriptor_type/descriptor_index control header), encoded in the
    /// CONTROL's own element format. The caller reads the descriptor
    /// first — the facade does not re-validate against value_details.
    statusbar::sg14::inplace_vector<uint8_t, 508> control_values{};
};

/// Kind of controller action — every command a driver can issue.
enum class ControllerActionKind : uint8_t
{
    DiscoverAll,            ///< Send broadcast ENTITY_DISCOVER (+ RX-state probe only if auto-probe enabled)
    ConnectStream,          ///< ACMP CONNECT_TX_COMMAND
    DisconnectStream,       ///< ACMP DISCONNECT_TX_COMMAND
    ReadEntityDescriptors,  ///< Read all descriptors for entity in request.talker_entity_id
    IdentifyEntity,         ///< SET_CONTROL on the entity's advertised identify control (request.identify_state)
    StartStreaming,         ///< Start streaming on stream (desc_type + desc_index)
    StopStreaming,          ///< Stop streaming on stream (desc_type + desc_index)
    SetStreamFormat,        ///< Set stream format (desc_type + desc_index + stream_format)
    SetClockSource,         ///< AEM SET_CLOCK_SOURCE (target=talker_entity_id, desc_index=clock_domain, clock_source_index)
    GetClockSource,         ///< AEM GET_CLOCK_SOURCE (target=talker_entity_id, desc_index=clock_domain)
    ConnectTxStream,        ///< ACMP CONNECT_TX_COMMAND (direct to talker; self-heal)
    DisconnectTxStream,     ///< ACMP DISCONNECT_TX_COMMAND (direct to talker; self-heal)
    GetCounters,            ///< AEM GET_COUNTERS (target=talker_entity_id, desc_type + desc_index)
    SetSignalSelector,      ///< AEM SET_SIGNAL_SELECTOR (target=talker_entity_id, desc_index, signal_type/index/output)
    GetSignalSelector,      ///< AEM GET_SIGNAL_SELECTOR (target=talker_entity_id, desc_index)
    GetControl,             ///< AEM GET_CONTROL (target=talker_entity_id, desc_index = CONTROL index)
    SetControl,             ///< AEM SET_CONTROL (target=talker_entity_id, desc_index, control_values)
    RegisterUnsolicited,    ///< AEM REGISTER_UNSOLICITED_NOTIFICATION (target=talker_entity_id)
    GetMatrix,              ///< AEM GET_MATRIX (target=talker_entity_id, desc_index, control_values = region request)
    SetMatrix,              ///< AEM SET_MATRIX (target=talker_entity_id, desc_index, control_values = region write)
    SetName,                ///< AEM SET_NAME (target=talker_entity_id, desc_type + desc_index, name_index 0,
                            ///<   configuration 0; control_values carries the UTF-8 name bytes, <= 64)
};

/// An action the driver wants the controller facade to perform.
struct ControllerAction
{
    ControllerActionKind kind{ControllerActionKind::DiscoverAll};
    StreamRequest request{};
};

//
// Data snapshot types (returned from controller facade queries)
//

/// Display information for a single discovered entity.
struct EntityDisplayInfo
{
    ieee::Eui64 entity_id{};
    std::string name;
    uint16_t talker_stream_sources{0};
    uint16_t listener_stream_sinks{0};
    bool has_talker{false};
    bool has_listener{false};
    std::vector<std::string> talker_stream_formats;
    std::vector<std::string> listener_stream_formats;
    std::vector<std::string> talker_stream_names;
    std::vector<std::string> listener_stream_names;
};

/// An active stream connection observed on the network.
struct ActiveConnection
{
    ieee::Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};
    ieee::Eui64 listener_entity_id{};
    uint16_t listener_unique_id{0};
    std::string talker_name;
    std::string listener_name;
    std::string talker_format;

    auto operator<=>(ActiveConnection const& o) const noexcept
    {
        if (auto c = talker_entity_id <=> o.talker_entity_id; c != 0) {
            return c;
        }
        if (auto c = talker_unique_id <=> o.talker_unique_id; c != 0) {
            return c;
        }
        if (auto c = listener_entity_id <=> o.listener_entity_id; c != 0) {
            return c;
        }
        return listener_unique_id <=> o.listener_unique_id;
    }
    auto operator==(ActiveConnection const& o) const noexcept -> bool
    {
        return talker_entity_id == o.talker_entity_id && talker_unique_id == o.talker_unique_id &&
            listener_entity_id == o.listener_entity_id && listener_unique_id == o.listener_unique_id;
    }
};

/// A single line in the entity detail view.
struct DetailLine
{
    std::string text;
    bool bold{false};  ///< Section header hint — consumer decides how to render
};

/// Accumulated descriptor information for a single entity.
struct EntityDetail
{
    ieee::Eui64 entity_id{};
    std::string name;
    std::vector<DetailLine> lines;
};

//
// Event types (asynchronous notifications from the controller facade)
//

struct ConnectionAddedEvent
{
    ActiveConnection connection;
};

struct ConnectionRemovedEvent
{
    ieee::Eui64 listener_entity_id{};
    uint16_t listener_unique_id{0};
};

struct EntityDetailReadyEvent
{
    EntityDetail detail;
};

struct StatusChangedEvent
{
    std::string status;
};

/// Outcome of one tracked AEM command — the typed completion surface.
/// Scriptable callers key on (entity_id, command_type, delivery, aem_status)
/// instead of matching StatusChangedEvent text. `response` is the response
/// payload copied out of the state machine's buffer.
struct CommandCompletedEvent
{
    ieee::Eui64 entity_id{};
    uint16_t command_type{0};
    atdecc::AemCommandDelivery delivery{atdecc::AemCommandDelivery::SendFailed};
    uint8_t aem_status{0};  ///< AEM_STATUS_* (meaningful when delivery == Responded)
    statusbar::sg14::inplace_vector<uint8_t, atdecc::AemInflightCommand::MAX_PAYLOAD> response;

    /// True when the command was answered with AEM_STATUS_SUCCESS.
    [[nodiscard]] auto ok() const noexcept -> bool
    {
        return delivery == atdecc::AemCommandDelivery::Responded && aem_status == atdecc::AEM_STATUS_SUCCESS;
    }
};

/// Emitted for every ACMP GET_RX_STATE_RESPONSE received (whether or not the
/// listener sink is connected), so a diagnostic caller can distinguish "entity
/// answered" from "timed out" — the exact signal macOS's avbdiagnose reports as
/// kIOReturnTimeout when our listener fails to reply.
struct RxStateEvent
{
    ieee::Eui64 listener_entity_id{};
    uint16_t listener_unique_id{0};
    uint8_t status{0};  // ACMP_STATUS_*
    bool connected{false};
    ieee::Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};
};

/// Emitted for every AEM GET_COUNTERS_RESPONSE received, carrying the raw
/// counters_valid bitmap and 32 counter values so a supervise/diagnostic
/// caller can sample a specific counter (e.g. STREAM_INPUT FRAMES_RX or
/// STREAM_OUTPUT FRAMES_TX) across two passes to detect a stalled stream.
struct CountersReadyEvent
{
    ieee::Eui64 entity_id{};
    uint16_t descriptor_type{0};
    uint16_t descriptor_index{0};
    uint32_t counters_valid{0};
    std::array<uint32_t, 32> counters{};
};

/// Emitted for EVERY ACMP PDU the controller observes on the wire — commands as
/// well as responses — so a diagnostic caller can reconstruct a connection
/// handshake leg by leg. Unlike the higher-level events above, this is the raw
/// message_type/status and the four endpoint fields exactly as seen on the bus.
/// A passive controller sees all of: the listener's relayed CONNECT_TX_COMMAND,
/// the talker's CONNECT_TX_RESPONSE, and the listener's CONNECT_RX_RESPONSE
/// (all multicast), which is enough to pinpoint which leg of a connect stalled.
struct AcmpTraceEvent
{
    uint8_t message_type{0};  // ACMP_MESSAGE_TYPE_*
    uint8_t status{0};        // ACMP_STATUS_*
    ieee::Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};
    ieee::Eui64 listener_entity_id{};
    uint16_t listener_unique_id{0};
};

/// An unsolicited AEM response (U bit set): a change made by another
/// controller or by the entity itself, delivered after RegisterUnsolicited.
/// `response` is the response payload after the AemDu header (e.g. a
/// SET_CONTROL response body: 4-byte control header + values).
struct UnsolicitedEvent
{
    ieee::Eui64 entity_id{};
    uint16_t command_type{0};
    uint8_t aem_status{0};
    statusbar::sg14::inplace_vector<uint8_t, atdecc::AemInflightCommand::MAX_PAYLOAD> response;
};

/// Variant covering every event the controller facade can emit.
using ControllerEvent = std::variant<
    ConnectionAddedEvent,
    ConnectionRemovedEvent,
    EntityDetailReadyEvent,
    StatusChangedEvent,
    CommandCompletedEvent,
    RxStateEvent,
    CountersReadyEvent,
    AcmpTraceEvent,
    UnsolicitedEvent>;

}  // namespace statusbar::atdecc_tools
