#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AEM Controller State Machine - sends AEM commands and tracks responses
/// Handles IN_PROGRESS timeout extension per IEEE 1722.1 Clause 9.2.1.2.5

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/container/container_slot_table.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace statusbar::atdecc {

using statusbar::container::SlotTable;

//
// AEM Inflight Command
//

/// How a tracked AEM command concluded.
enum class AemCommandDelivery : uint8_t
{
    Responded,   ///< final response received; `status` and `response` are valid
    TimedOut,    ///< inflight timeout expired with no final response
    SendFailed,  ///< never went on the wire (inflight table full or tx failed)
};

/// The outcome of one tracked AEM command, delivered to its completion.
/// The spans reference the state machine's buffers and are only valid for
/// the duration of the completion call — copy what must outlive it.
struct AemCommandResult
{
    AemCommandDelivery delivery{AemCommandDelivery::SendFailed};
    uint8_t status{0};  ///< AEM_STATUS_* (meaningful when delivery == Responded)
    uint16_t command_type{0};
    ieee::Eui64 target_entity_id{};
    std::span<uint8_t const> sent_payload{};  ///< the original request payload
    std::span<uint8_t const> response{};      ///< response payload (empty unless Responded)
};

/// Per-command completion: fires exactly once per tracked command — on the
/// final response, on timeout, or immediately when the send fails. This is
/// the correlation mechanism; callers never match sequence IDs themselves.
using AemCommandCompletion = statusbar::sg14::inplace_function<void(AemCommandResult const&), 64>;

/// Tracks a sent AEM command waiting for a response.
///
/// Slot occupancy is tracked by the enclosing SlotTable (whose size()
/// is the source of truth), so there is no longer a `bool valid` field.
struct AemInflightCommand
{
    static constexpr size_t MAX_PAYLOAD = 512;

    AemDu sent_header{};                                            ///< The AemDu header as sent
    statusbar::sg14::inplace_vector<uint8_t, MAX_PAYLOAD> payload;  ///< Command payload copy
    AemCommandTracker tracker;                                      ///< IN_PROGRESS timeout tracker
    AemCommandCompletion completion;                                ///< fires once with the outcome (may be unset)
};

//
// AEM Controller Context
//

/// Parameters for building an AEM command
struct AemCommandParams
{
    ieee::Eui64 target_entity_id{};
    uint16_t command_code{0};
    std::span<uint8_t const> command_data{};
    AemCommandCompletion completion{};  ///< optional per-command outcome callback
};

/// Context for the AEM controller state machine
struct AemControllerContext
{
    using TimePoint = sm::TimePoint;
    static constexpr size_t MAX_INFLIGHT = 8;

    /// Entity ID of this controller
    ieee::Eui64 my_id{};

    /// Sequence ID counter
    uint16_t next_sequence_id{0};

    /// Command to send (set before DoCommand event)
    AemCommandParams command_params{};

    /// Received response being processed
    AemDu rcvd_header{};
    std::span<uint8_t const> rcvd_response_data{};

    /// Index of current inflight being processed
    size_t current_inflight_index{0};

    /// Inflight commands — zero-heap fixed-capacity table.
    SlotTable<AemInflightCommand, MAX_INFLIGHT> inflight{};

    // Callbacks

    /// Send a serialized AEM packet (AemDu header + payload)
    statusbar::sg14::inplace_function<bool(std::span<uint8_t const> packet), 64> tx_command;

    /// Called on final (non-IN_PROGRESS) response.
    /// `sent_payload` is the original request payload (after the AemDu header)
    /// — useful when the response payload echoes are unreliable (some entities
    /// zero them out on error responses).
    statusbar::sg14::inplace_function<
        void(AemDu const& header, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data, uint8_t status),
        64>
        on_response;

    /// Called when a command times out
    statusbar::sg14::inplace_function<void(AemDu const& header), 64> on_timeout;

    // Inflight management — thin wrappers over the SlotTable so existing
    // call sites keep compiling unchanged.

    /// Find inflight by sequence_id. Returns `MAX_INFLIGHT` if not found.
    [[nodiscard]] auto find_inflight(uint16_t sequence_id) const -> size_t
    {
        return inflight.find_if(
            [sequence_id](AemInflightCommand const& e) { return e.sent_header.sequence_id.get() == sequence_id; });
    }

    /// Find first timed-out inflight. Returns `MAX_INFLIGHT` if none.
    [[nodiscard]] auto find_timed_out(TimePoint now) const -> size_t
    {
        return inflight.find_if([now](AemInflightCommand const& e) { return e.tracker.check_timeout(now); });
    }

    /// Add a new inflight command. Returns true if added, false if the
    /// table is at capacity. Callers must check this return value before
    /// committing the associated packet to the wire.
    auto add_inflight(
        AemDu const& header, std::span<uint8_t const> payload_data, TimePoint now, AemCommandCompletion completion = {}) -> bool
    {
        AemInflightCommand entry{};
        entry.sent_header = header;
        auto const copy_size = std::min(payload_data.size(), AemInflightCommand::MAX_PAYLOAD);
        entry.payload.assign(payload_data.begin(), payload_data.begin() + static_cast<ptrdiff_t>(copy_size));
        entry.tracker.start(header.sequence_id.get(), header.command_code(), now);
        entry.completion = std::move(completion);
        return inflight.add(entry);
    }

    /// Remove an inflight command by index. No-op if out of range.
    void remove_inflight(size_t index)
    {
        if (auto* entry = inflight.get(index); entry != nullptr) {
            entry->tracker.cancel();
        }
        inflight.remove(index);
    }

    /// Count of active inflight commands.
    [[nodiscard]] auto inflight_count() const -> size_t { return inflight.size(); }
};

//
// AEM Controller State Machine
//

enum class AemControllerState : uint8_t
{
    Start,
    Waiting,
    Command,
    Response,
    Timeout,
    Count
};

enum class AemControllerEvent : uint8_t
{
    UCT,
    DoCommand,
    RcvdResponse,
    Timeout,
    Count
};

namespace aem_controller_actions {

/// Build and send an AEM command
void send_aem_command(AemControllerContext& ctx, sm::TimePoint event_time);

/// Process a received AEM response (may be IN_PROGRESS)
void handle_aem_response(AemControllerContext& ctx, sm::TimePoint event_time);

/// Handle a timed-out AEM command
void handle_aem_timeout(AemControllerContext& ctx, sm::TimePoint event_time);

}  // namespace aem_controller_actions

struct AemControllerDef
{
    using Context = AemControllerContext;
    using State = AemControllerState;
    using Event = AemControllerEvent;
};

consteval auto make_aem_controller_table()
{
    using T = sm::Transitions<AemControllerDef>;
    using State = AemControllerState;
    using Event = AemControllerEvent;

    sm::TransitionTable<AemControllerDef> t{};

    t.at(State::Start, Event::UCT) = T::transition(State::Waiting);

    t.at(State::Waiting, Event::DoCommand) = T::transition(State::Command);
    t.at(State::Waiting, Event::RcvdResponse) = T::transition(State::Response);
    t.at(State::Waiting, Event::Timeout) = T::transition(State::Timeout);

    t.at(State::Command, Event::UCT) = T::action<aem_controller_actions::send_aem_command>(State::Waiting);
    t.at(State::Response, Event::UCT) = T::action<aem_controller_actions::handle_aem_response>(State::Waiting);
    t.at(State::Timeout, Event::UCT) = T::action<aem_controller_actions::handle_aem_timeout>(State::Waiting);

    return t;
}

inline constexpr auto aem_controller_table = make_aem_controller_table();

template <typename Observer = sm::NullObserver>
using AemControllerStateMachine = sm::StateMachine<AemControllerDef, aem_controller_table, Observer>;

}  // namespace statusbar::atdecc
