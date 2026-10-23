#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// TalkerDeclarations: when each talker stream output declares its MSRP Talker Advertise
/// (Milan v1.2 Sections 4.3.3.1, 5.3.7.2, 5.3.7.5). Pure state and time (steady-clock ns passed
/// in); reactor thread only. See docs/MILAN_TALKER.md.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace statusbar::avb_entity {

class TalkerDeclarations
{
  public:
    /// Section 4.3.3.1: a probe keeps the stream declared this long.
    static constexpr int64_t PROBE_WINDOW_NS = 15'000'000'000;
    /// Table 5.3: 2 LeaveAll periods, at the longest LeaveAll the MRP timers
    /// pick (srp_mrp_timers.hpp: uniform in [10 s, 15 s); Milan's 15.5 s
    /// upper tolerance is GenAVB's MRP_LVATIMER_VAL_MAX).
    static constexpr int64_t REDECLARE_HOLD_NS = 2 * 15'000'000'000;
    /// How long a departed Listener still counts as registered: the transmit
    /// gate's grace window (TalkerGate::GRACE_NS), across a LeaveAll blip.
    static constexpr int64_t LISTENER_GRACE_NS = 5'000'000'000;
    static constexpr size_t STREAM_COUNT = MAX_ENTITY_STREAMS;

    /// A Talker Advertise's identity (its FirstValue's stream ID, destination
    /// address and VID): what the hold compares.
    struct Params
    {
        ieee::Eui64 stream_id{};
        ieee::Eui48 dest_mac{};
        uint16_t vlan_id{0};
        auto operator<=>(Params const&) const noexcept = default;
    };

    /// What evaluate() asks of a stream.
    enum class Action : uint8_t
    {
        none,      ///< leave it as it is
        declare,   ///< declare (or re-declare) its Talker Advertise
        withdraw,  ///< withdraw it
    };

    explicit TalkerDeclarations(bool const interest_required) noexcept
        : interest_required_{interest_required}
    {}

    /// The control plane advertises (true) or has withdrawn everything (false).
    void set_active(bool const active) noexcept { active_ = active; }
    [[nodiscard]] auto active() const noexcept -> bool { return active_; }

    /// A PROBE_TX_COMMAND for stream @p idx counted at @p now_ns: its window
    /// runs to now + PROBE_WINDOW_NS.
    void note_probe(uint16_t idx, int64_t now_ns) noexcept;
    /// Whether a Listener attribute (any declaration type) for stream @p idx's stream ID is
    /// registered, seen at @p now_ns. A departed Listener keeps condition 2 true for
    /// LISTENER_GRACE_NS (the MRP LeaveAll re-registration blip).
    void note_listener_registered(uint16_t idx, bool registered, int64_t now_ns) noexcept;
    /// Whether condition 2's Listener arm holds for stream @p idx at @p now_ns:
    /// registered, or gone less than LISTENER_GRACE_NS ago.
    [[nodiscard]] auto listener_interest(uint16_t idx, int64_t now_ns) const noexcept -> bool;
    /// Whether MAAP has a conflict-free destination address for stream @p idx
    /// (always true with static addresses).
    void note_address_valid(uint16_t idx, bool valid) noexcept;

    /// Whether stream @p idx's probe window is open at @p now_ns.
    [[nodiscard]] auto probe_window_open(uint16_t idx, int64_t now_ns) const noexcept -> bool;
    /// Section 4.3.3.1's predicate for stream @p idx at @p now_ns (and the
    /// control plane active): its SRP parameters are valid.
    [[nodiscard]] auto wants_declared(uint16_t idx, int64_t now_ns) const noexcept -> bool;

    /// What stream @p idx needs at @p now_ns, its Talker Advertise now being @p params. @p refresh
    /// re-declares changed parameters (the adopted Class A VID) without a hold; otherwise a
    /// declared stream whose parameters changed is withdrawn first and held.
    [[nodiscard]] auto evaluate(uint16_t idx, int64_t now_ns, Params const& params, bool refresh) const noexcept -> Action;

    /// The kit declared stream @p idx with @p params.
    void note_declared(uint16_t idx, Params const& params) noexcept;
    /// The kit withdrew stream @p idx at @p now_ns: the hold starts.
    void note_withdrawn(uint16_t idx, int64_t now_ns) noexcept;
    /// The control plane withdrew every stream (no Talker Advertise left) at
    /// @p now_ns: each declared one is held as by note_withdrawn.
    void note_all_withdrawn(int64_t now_ns) noexcept;

    [[nodiscard]] auto declared(uint16_t idx) const noexcept -> bool { return idx < STREAM_COUNT && streams_[idx].declared; }
    [[nodiscard]] auto address_valid(uint16_t idx) const noexcept -> bool
    {
        return idx < STREAM_COUNT && streams_[idx].address_valid;
    }
    [[nodiscard]] auto listener_registered(uint16_t idx) const noexcept -> bool
    {
        return idx < STREAM_COUNT && streams_[idx].listener_registered;
    }

    /// The next time evaluate() may decide differently with nothing else
    /// happening (a probe window closing on a declared stream, or a hold
    /// ending), after @p now_ns; nullopt when none.
    [[nodiscard]] auto next_deadline(int64_t now_ns) const noexcept -> std::optional<int64_t>;

  private:
    struct Stream
    {
        bool declared{false};
        bool listener_registered{false};
        bool address_valid{false};
        int64_t probe_until_ns{0};  ///< 0: never probed
        int64_t listener_seen_ns{0};  ///< the last moment a Listener was known registered (0: never)
        Params declared_params{};
        /// What was last withdrawn, and until when a different declaration waits.
        Params withdrawn_params{};
        int64_t hold_until_ns{0};
    };

    /// Table 5.3 holds a new destination address (or stream ID) after a
    /// withdrawal, in the Milan profile; a new VID is the control plane's own
    /// choice (the adopted Class A Domain) and is not held.
    [[nodiscard]] auto held(Stream const& s, int64_t now_ns, Params const& params) const noexcept -> bool
    {
        return interest_required_ && now_ns < s.hold_until_ns &&
            (params.stream_id != s.withdrawn_params.stream_id || params.dest_mac != s.withdrawn_params.dest_mac);
    }

    bool interest_required_;
    bool active_{false};
    std::array<Stream, STREAM_COUNT> streams_{};
};

}  // namespace statusbar::avb_entity
