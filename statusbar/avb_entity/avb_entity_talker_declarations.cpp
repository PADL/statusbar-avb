// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_talker_declarations.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace statusbar::avb_entity {

void TalkerDeclarations::notify_probe(uint16_t const idx, int64_t const now_ns) noexcept
{
    if (idx < STREAM_COUNT) {
        streams_[idx].probe_until_ns = now_ns + PROBE_WINDOW_NS;
    }
}

void TalkerDeclarations::notify_listener_registered(uint16_t const idx, bool const registered) noexcept
{
    if (idx < STREAM_COUNT) {
        streams_[idx].listener_registered = registered;
    }
}

void TalkerDeclarations::notify_address_valid(uint16_t const idx, bool const valid) noexcept
{
    if (idx < STREAM_COUNT) {
        streams_[idx].address_valid = valid;
    }
}

auto TalkerDeclarations::probe_window_open(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    return idx < STREAM_COUNT && streams_[idx].probe_until_ns != 0 && now_ns < streams_[idx].probe_until_ns;
}

auto TalkerDeclarations::wants_declared(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    if (idx >= STREAM_COUNT || !active_) {
        return false;
    }
    auto const& s = streams_[idx];
    return s.address_valid && (!interest_required_ || s.listener_registered || probe_window_open(idx, now_ns));
}

auto TalkerDeclarations::evaluate(uint16_t const idx, int64_t const now_ns, Params const& params, bool const refresh) const noexcept
    -> Action
{
    if (idx >= STREAM_COUNT) {
        return Action::none;
    }
    auto const& s = streams_[idx];
    if (!wants_declared(idx, now_ns)) {
        return s.declared ? Action::withdraw : Action::none;
    }
    if (s.declared) {
        if (params == s.declared_params || refresh) {
            return refresh ? Action::declare : Action::none;
        }
        // New parameters the control plane did not choose (a new MAAP address
        // without the conflict seen): a Milan stream withdraws, then waits out
        // the hold; a classic one re-declares.
        return interest_required_ ? Action::withdraw : Action::declare;
    }
    return held(s, now_ns, params) ? Action::none : Action::declare;
}

void TalkerDeclarations::notify_declared(uint16_t const idx, Params const& params) noexcept
{
    if (idx < STREAM_COUNT) {
        streams_[idx].declared = true;
        streams_[idx].declared_params = params;
    }
}

void TalkerDeclarations::notify_withdrawn(uint16_t const idx, int64_t const now_ns) noexcept
{
    if (idx >= STREAM_COUNT || !streams_[idx].declared) {
        return;
    }
    auto& s = streams_[idx];
    s.declared = false;
    s.withdrawn_params = s.declared_params;
    s.hold_until_ns = now_ns + REDECLARE_HOLD_NS;
}

void TalkerDeclarations::notify_all_withdrawn(int64_t const now_ns) noexcept
{
    for (uint16_t idx = 0; idx < STREAM_COUNT; ++idx) {
        notify_withdrawn(idx, now_ns);
    }
}

auto TalkerDeclarations::next_deadline(int64_t const now_ns) const noexcept -> std::optional<int64_t>
{
    std::optional<int64_t> next{};
    auto const consider = [&next, now_ns](int64_t const at) {
        if (at > now_ns) {
            next = next ? std::min(*next, at) : at;
        }
    };
    for (auto const& s : streams_) {
        if (s.declared && interest_required_ && !s.listener_registered) {
            consider(s.probe_until_ns);  // the window closing withdraws it
        }
        if (!s.declared) {
            consider(s.hold_until_ns);  // a held declaration may go ahead
        }
    }
    return next;
}

}  // namespace statusbar::avb_entity
