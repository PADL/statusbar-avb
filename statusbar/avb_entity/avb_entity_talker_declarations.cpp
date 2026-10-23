// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_talker_declarations.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace statusbar::avb_entity {

void TalkerDeclarations::note_probe(uint16_t const idx, int64_t const now_ns) noexcept
{
    if (idx < STREAM_COUNT) {
        streams_[idx].probe_until_ns = now_ns + PROBE_WINDOW_NS;
    }
}

void TalkerDeclarations::note_listener_registered(uint16_t const idx, bool const registered, int64_t const now_ns) noexcept
{
    if (idx >= STREAM_COUNT) {
        return;
    }
    auto& s = streams_[idx];
    // Registered now, or registered until now: the grace runs from the last
    // moment it was known registered (a repeated "gone" moves nothing).
    if (registered || s.listener_registered) {
        s.listener_seen_ns = now_ns;
    }
    s.listener_registered = registered;
}

auto TalkerDeclarations::listener_interest(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    if (idx >= STREAM_COUNT) {
        return false;
    }
    auto const& s = streams_[idx];
    return s.listener_registered || (s.listener_seen_ns != 0 && now_ns - s.listener_seen_ns < LISTENER_GRACE_NS);
}

void TalkerDeclarations::note_address_valid(uint16_t const idx, bool const valid) noexcept
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
    return s.address_valid && (!interest_required_ || listener_interest(idx, now_ns) || probe_window_open(idx, now_ns));
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

void TalkerDeclarations::note_declared(uint16_t const idx, Params const& params) noexcept
{
    if (idx < STREAM_COUNT) {
        streams_[idx].declared = true;
        streams_[idx].declared_params = params;
    }
}

void TalkerDeclarations::note_withdrawn(uint16_t const idx, int64_t const now_ns) noexcept
{
    if (idx >= STREAM_COUNT || !streams_[idx].declared) {
        return;
    }
    auto& s = streams_[idx];
    s.declared = false;
    s.withdrawn_params = s.declared_params;
    s.hold_until_ns = now_ns + REDECLARE_HOLD_NS;
}

void TalkerDeclarations::note_all_withdrawn(int64_t const now_ns) noexcept
{
    for (uint16_t idx = 0; idx < STREAM_COUNT; ++idx) {
        note_withdrawn(idx, now_ns);
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
            if (s.listener_seen_ns != 0) {
                consider(s.listener_seen_ns + LISTENER_GRACE_NS);  // ... or the departed Listener's grace ending
            }
        }
        if (!s.declared) {
            consider(s.hold_until_ns);  // a held declaration may go ahead
        }
    }
    return next;
}

}  // namespace statusbar::avb_entity
