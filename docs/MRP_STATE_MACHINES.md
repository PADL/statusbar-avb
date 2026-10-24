<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# MRP / MSRP / MVRP State Machines

This document describes the IEEE 802.1Q-2014 Multiple Registration Protocol
(MRP) implementation in `statusbar/tsn/` and how it is used by the
MSRP (Clause 35) and MVRP (Clause 11.2) participant classes.

## Scope

- **Local endpoint only.** Not bridge-capable. A single participant
  instance represents one end of a point-to-point MRP relationship on
  one NIC (one 802.1Q "port"). Per-port state; no forwarding logic.
- **No daemon, no sockets, no management surface.** The participant is
  an in-process library. PDU I/O is via user-supplied callbacks
  (`set_send_pdu`) and explicit `receive_pdu()` calls. Timers are
  driven by `tick()` and exposed via `next_deadline()` for integration
  with any event loop.
- **No MMRP.** MAC multicast registration is out of scope for this
  endpoint implementation.
- **`operPointToPointMAC = true`** is hardcoded, which collapses the
  shared-media branches in the Applicant FSM's `rJoinIn!` and `rIn!`
  transitions per Clause 10.7.7 Table 10-3 notes 4-7. A point-to-point
  link is the only supported topology for a local AVB endpoint.

Reference implementation during porting: OpenAvnu mrpd at
`stash/avnu/OpenAvnu/daemons/mrpd/`. Line-number citations throughout
the code point back to `mrpd/mrp.c` for auditability.

## Module layout

```
statusbar/tsn/
├── mrp.hpp                     wire-format primitives (pre-existing)
├── msrp.hpp, mvrp.hpp          FirstValue wire-format structs
├── mrp_applicant_sm.hpp/.cpp   12-state Applicant FSM (Clause 10.7.7)
├── mrp_registrar_sm.hpp/.cpp   3-state Registrar FSM (Clause 10.7.8)
├── mrp_leaveall_sm.hpp/.cpp    2-state LeaveAll FSM (Clause 10.7.5.22)
├── mrp_periodic_sm.hpp/.cpp    2-state PeriodicTransmission FSM (10.7.5.23)
├── mrp_attribute.hpp           per-attribute record: FirstValue + both FSMs
├── mrp_timers.hpp/.cpp         TimerScheduler (monotonic, no OS timers)
├── mrp_participant.hpp/.cpp    PortState + generic dispatch helpers
├── msrp_participant.hpp/.cpp   MSRP specialization + Observer API
└── mvrp_participant.hpp/.cpp   MVRP specialization + Observer API
```

## The four state machines

All four FSMs are expressed as compile-time `TransitionTable<Def>` constants
using the `statusbar::sm` framework. Each transition is a
`Transition{next_state, action_fn, valid}` cell in a 2D array indexed by
`[State][Event]`. Invalid cells (no table entry) result in a no-op —
equivalent to the `default: break;` fall-through used by `mrp.c`.

### Applicant FSM — `mrp_applicant_sm.hpp`

- **12 states** (per Clause 10.7.7 Table 10-2): `Vo`, `Vp`, `Vn`, `An`, `Aa`, `Qa`,
  `La`, `Ao`, `Qo`, `Ap`, `Qp`, `Lo`. Plus a `Start` pseudo-state that
  transitions via UCT to `Vo` on BEGIN!
- **Events**: `New`, `Join`, `Leave`, `TxLeaveAll`, `TxLeaveAllFull`,
  `RNew`, `RJoinIn`, `RIn`, `RJoinMt`, `RMt`, `RLeave`, `RLeaveAll`,
  `Redeclare`, `Periodic`, plus the two `Tx` variants described below.
- **One cross-FSM coupling.** Clause 10.7.7 Note 8 specifies that on
  the `tx!` event from state `An`, the next state depends on whether
  the paired *Registrar* is in `In`. With `p2pmac = true` this is the
  only runtime-dependent transition in the entire Applicant FSM. It is
  resolved by splitting the single `tx!` event into two variants:
  - `TxRegistrarIn` — fired when the paired Registrar is `In`
  - `TxRegistrarMt` — fired when the paired Registrar is `Lv` or `Mt`
  The participant driver picks which variant to dispatch; only the
  `An` cell differs between the two columns.
- **Side-effect outputs.** Each transmit-producing transition writes
  three fields into `applicant_sm::Context`:
  - `tx_pending` (bool) — this attribute wants to be included in the
    next outgoing PDU
  - `send_msg` (enum) — internal sndmsg intent: `New` / `Join` / `In` / `Leave`
  - `encode` (enum) — `Required` or `Optional`
  The `send_msg` is an internal code, not a wire `AttributeEvent`. The
  participant's transmit path translates it at encode time using the
  paired Registrar's current state (`Join` → `JoinIn` / `JoinMt`; `In` → `In` / `Mt`).
  This preserves `mrp.c`'s two-phase encoding and keeps the two FSMs
  loosely coupled.

See `mrp_applicant_sm.hpp` for the full table, cross-referenced to
`mrp.c:551-904` line ranges per event.

### Registrar FSM — `mrp_registrar_sm.hpp`

- **3 states**: `In`, `Lv`, `Mt`. Plus `Start` (UCT → `Mt`).
- **Events**: `RNew`, `RJoinIn`, `RJoinMt`, `RLeave`, `RLeaveAll`,
  `TxLeaveAll`, `Redeclare`, `LvTimer`, `Flush`. (`rIn!` and `rMt!`
  are intentionally ignored per Clause 10.7.8.)
- **Side-effect outputs** in `registrar_sm::Context`:
  - `notify` (enum) — `None` / `New` / `Join` / `Leave`
  - `lvtimer_request` (bool) — when true, the participant starts the
    per-port LeaveTimer (idempotent if already running)
- **Asymmetric self-loop behaviour.** A subtle point inherited from
  `mrp.c`: `rNew!` on state `In` *does* set `notify = New` (self-loop
  with a side effect). `rJoinIn!` / `rJoinMt!` on state `In` does *not*
  set `notify` (silent refresh). This asymmetry is preserved
  intentionally — the rNew self-loop is encoded in the table with an
  action; the rJoinIn/rJoinMt IN cell is simply absent.

### LeaveAll Timer FSM — `mrp_leaveall_sm.hpp`

- **2 states**: `Passive`, `Active`. Plus `Start` (UCT → `Passive`).
- One instance per port per protocol. Periodically flips to `Active`,
  at which point the next TX pass sends the PDU with the LeaveAll
  flag set, then flips back to `Passive`.
- LeaveAll interval is randomized in `[0.5×LAT, 1.5×LAT)` per Clause
  10.7.4.2, using a `TimerScheduler`-owned `std::mt19937_64` seeded
  deterministically for tests.

### Periodic Transmission FSM — `mrp_periodic_sm.hpp`

- **2 states**: `Passive`, `Active`. Plus `Start` (UCT → `Active`).
- One instance per port per protocol. Controls whether the 1-second
  Periodic timer is rearmed after each fire. When `Active`, each
  Periodic expiry dispatches a `Periodic` event to every attribute's
  Applicant, which triggers the `Qa → Aa` and `Qp → Ap` transitions
  that keep quiet attributes re-announcing.

## Timer scheduler — `mrp_timers.hpp`

`TimerScheduler` tracks four deadline-based timers as `std::optional<TimePoint>`
fields:

| Timer       | Duration                 | Clause       |
|-------------|--------------------------|--------------|
| JoinTimer   | 100 ms                   | 10.7.4.1     |
| LeaveTimer  | 1000 ms                  | 10.7.4.3     |
| LeaveAllTimer | randomized 5–15 s      | 10.7.4.2     |
| PeriodicTimer | 1000 ms                | 10.7.5.23    |

### Operations

- `start_*(now)` — idempotent; no-op if already running (matches `mrp.c`)
- `arm_*(now)` — force-(re)arm; always resets the deadline
- `stop_*()` — stops if running
- `next_deadline()` — earliest deadline across all running timers, or
  `TimePoint::max()` if none. Use this to schedule a `poll`/`epoll`
  wait in the caller's event loop.
- `tick(now)` — returns an `Expired` struct reporting which timers
  have expired; expired timers are automatically cleared (the caller
  is responsible for rearming based on FSM outputs).

The scheduler is not thread-safe; a participant is assumed to run on
a single event-loop thread.

## Attribute record — `mrp_attribute.hpp`

```cpp
template <typename FirstValue>
struct AttributeRecord {
    FirstValue first_value;              // wire-format payload
    Operation operation;                 // Register | Declare
    applicant_sm::Context applicant_ctx; // tx_pending, send_msg, encode
    applicant_sm::Machine applicant_sm;  // current state
    registrar_sm::Context registrar_ctx; // notify, lvtimer_request
    registrar_sm::Machine registrar_sm;  // current state
    bool should_reclaim;
};
```

Helpers: `registrar_is_in()` for picking the TX event variant, and
`is_dead()` (`Applicant == Vo && Registrar == Mt`) for reclaim sweeps.

MSRP's Listener type needs an extra `ListenerDeclaration substate`
field and therefore uses a dedicated `ListenerRecord` struct that
satisfies the same `MrpAttributeLike` concept rather than reusing
`AttributeRecord<ListenerFirstValue>`.

## Participants

### `PortState` (generic, `mrp_participant.hpp`)

Composes the three per-port pieces that are shared across all
protocols:

- `TimerScheduler timers_`
- `leaveall_sm::Machine` / `Context`
- `periodic_sm::Machine` / `Context`

Plus event dispatch helpers that consume each FSM's side-effect
outputs and wire them back into the timer scheduler:

- `dispatch_leaveall(event, now)` — forwards `timer_restart` to
  `arm_leaveall()`
- `dispatch_periodic(event, now)` — forwards `timer_restart` to
  `arm_periodic()`

Per-attribute dispatch helpers (free templates constrained by the
`MrpAttributeLike` concept):

- `dispatch_applicant(rec, event, now)` — clears outputs then fires
- `dispatch_registrar(rec, event, now, timers)` — clears outputs, fires,
  then forwards `lvtimer_request` to `timers.start_leave()`
- `dispatch_applicant_tx(rec, now)` — automatically selects
  `TxRegistrarIn` vs `TxRegistrarMt` based on the paired Registrar's
  state (Clause 10.7.7 Note 8)

### `MsrpParticipant` (Clause 35, `msrp_participant.hpp`)

Holds one `PortState` plus four attribute vectors (TalkerAdvertise,
TalkerFailed, Listener, Domain). All capacities are resolved at
construction time via an explicit `MsrpConfig`:

```cpp
MsrpConfig cfg{
    .max_talker_advertise      = 16,
    .max_talker_failed         = 4,
    .max_listeners             = 16,
    .max_domains               = 2,
    .max_observers             = 4,
    .max_interesting_stream_ids = 16,
};
MsrpParticipant msrp{cfg};
```

Every container is `reserve()`d once during construction; the protocol
engine performs **zero heap allocations** on the steady-state TX / RX /
tick paths. Local declarations that would exceed a configured limit
return `TsnError::AttributeTableFull`; peer-observed attributes that
would exceed capacity are silently dropped (the peer's next periodic
cycle retransmits them if room later becomes available).

The outgoing PDU is built into a single fixed-size
`MutableBufferWithStorage<MAX_PDU_BYTES>` member owned by the
participant and reused on every TX pass — no per-call allocation for
the wire payload.

Exposes:

- **Local declaration API** — `declare_talker_advertise`,
  `declare_talker_failed`, `withdraw_talker`, `declare_listener`,
  `withdraw_listener`, `declare_domain`, `withdraw_domain`. Each
  returns `Status` that will be `TsnError::AttributeTableFull` if the
  backing table is at capacity.
- **Talker transmit authorization** — `listener_permits_transmit(stream_id)`
  returns `true` iff a peer listener declaration for this stream has
  been received with substate `Ready` or `ReadyFailed` *and* the
  Registrar for that peer record is in `In`. IEEE 802.1Q-2014
  Clause 35 forbids a talker from transmitting media frames until
  this predicate is true; use it to gate the audio pipeline.
- **Observer subscription** — `subscribe(Observer)` / `unsubscribe(id)`.
  `Observer` is a struct of `std::function` callbacks:
  - `on_talker_advertise(TalkerAdvertiseFirstValue, Operation)`
  - `on_talker_failed(TalkerFailedFirstValue, Operation)`
  - `on_talker_leave(StreamId)`
  - `on_listener(StreamId, ListenerDeclaration, Operation)`
  - `on_listener_leave(StreamId)`
  - `on_domain(DomainFirstValue, Operation)` / `on_domain_leave(DomainFirstValue)`
  The `Operation` argument distinguishes locally-declared attributes
  from peer-observed registrations.
- **PDU I/O** — `receive_pdu(span, now)` and `set_send_pdu(fn)`
- **Event loop** — `tick(now)` and `next_deadline()`
- **Interesting Stream ID pruning** — optional filter mirroring the
  `mrp.c` MSRP feature: when enabled, TalkerAdvertise / TalkerFailed
  attributes learned from peers are only retained if their `StreamId`
  is in the interesting set. Set methods:
  `set_pruning_enabled(bool)`, `add_interesting_stream_id(StreamId)`,
  `remove_interesting_stream_id(StreamId)`, `clear_interesting_stream_ids()`.

### `MvrpParticipant` (Clause 11.2, `mvrp_participant.hpp`)

Same shape as `MsrpParticipant` but with only a `VlanIdentifier`
attribute type and a smaller `MvrpConfig` (two fields: `max_vlans`,
`max_observers`). API surface:

- `declare_vlan(vid, now)` / `withdraw_vlan(vid, now)` — return
  `TsnError::InvalidVlanId` for out-of-range VIDs,
  `TsnError::AttributeTableFull` at capacity.
- `has_vlan(vid)` / `is_vlan_registered(vid)`
- Observer callbacks: `on_vlan_registered(vid, Operation)` /
  `on_vlan_leave(vid)`
- Same `receive_pdu` / `set_send_pdu` / `tick` / `next_deadline` shape
- Same zero-allocation steady state: attribute database, observer
  slots, and PDU build buffer are all reserved at construction.

## Event flow walkthrough

A full declare → peer notification → withdraw cycle:

1. **Local declare.**
   `a.declare_talker_advertise(fv, now)` — creates or updates the
   attribute record, fires `Applicant::New` (or `Join` if refreshing),
   starts the JoinTimer.
2. **Join timer expiry.**
   Caller's event loop observes `a.next_deadline()` has arrived,
   calls `a.tick(now)`. The scheduler reports the Join timer fired;
   the participant dispatches `TxRegistrarMt` (or `TxRegistrarIn`) to
   every attribute's Applicant, reads the resulting `tx_pending` /
   `send_msg` / `encode` outputs, and builds a PDU.
3. **PDU emit.**
   The built PDU is passed to the `send_pdu` callback installed by
   the caller. In a real deployment this writes to a raw Ethernet
   socket; in tests it enqueues into a `Fabric` helper.
4. **Peer receive.**
   On the other side, `b.receive_pdu(pdu, now)` walks the PDU
   structure (Clause 10.8.2: version, attribute list header, vector
   attribute headers, FirstValue, 3-packed events, 4-packed listener
   substates). For each attribute, it dispatches `RNew` / `RJoinIn` /
   `RJoinMt` / `RLv` etc. to both the Applicant and Registrar FSMs of
   the corresponding attribute record.
5. **Observer notification.**
   If the Registrar emits `notify = New | Join`, the participant calls
   every subscribed Observer's `on_talker_advertise` / `on_listener` /
   etc. with `Operation::Register`.
6. **Withdraw.**
   `a.withdraw_talker(stream_id, now)` fires `Applicant::Leave`,
   driving the attribute toward `La`. The next Join timer tick emits
   a PDU with an `Lv` wire event. On the peer side, the Registrar
   transitions `In → Lv` (with `lvtimer_request = true`, starting the
   LeaveTimer), and after the 1-second LeaveTimer expires, the
   Registrar transitions `Lv → Mt` with `notify = Leave`, and the
   observer's `on_talker_leave` fires.

## Key design decisions

- **`statusbar::sm` transition tables are pure constexpr** — no
  framework modifications were needed. The one cross-FSM coupling is
  handled by event variants dispatched by the driver, not by guard
  predicates or action return values.
- **Per-port, not multi-port.** Each NIC / port gets its own
  `MsrpParticipant` / `MvrpParticipant` instance, matching 802.1Q's
  "Applicant per port per attribute" model exactly.
- **Observer via `std::function` callbacks**, not virtual interface.
  Matches existing statusbar style; lets callers bind capture state
  into lambdas.
- **Timers via `tick()` + `next_deadline()`**, not OS timerfd. Caller
  owns the event loop and chooses poll/epoll/coroutines/etc.
- **Singleton vector encoding.** The participant emits one vector per
  attribute on transmit — no coalescing of contiguous stream IDs into
  a multi-value vector. Decode path handles multi-value receive
  correctly; encode coalescing can be added later if profiling shows
  PDU size is a concern.

## Testing

- **FSM-level tests** in `mrp_applicant_sm_test.cpp` and
  `mrp_registrar_sm_test.cpp` verify representative cells of each
  transition table against the ported `mrp.c` reference, including
  the critical `An` split cells and the p2pmac-collapse on `rJoinIn!`.
- **Integration tests** in `msrp_participant_test.cpp` exercise two
  participants back-to-back via a `Fabric` helper, covering
  declare→advertise→notify, withdraw→leave-timer→notify, listener
  declaration, interesting stream ID pruning, and observer
  subscription lifecycle.

## End-station and Milan additions

- **Immediate leave for MSRP.** Milan v1.2 Section 4.2.7.2.2 replaces the
  registrar's `IN / rLv! -> (start leavetimer) -> LV` with
  `IN / rLv! -> (Lv) -> MT` for MSRP (`Event::RLeaveImmediate`); Lv and Mt
  behave as for the standard rLv!. MVRP keeps the standard transition.
- **Timers.** LeaveTime defaults to 5000 ms (Milan v1.2 Table 4.3,
  IEEE 802.1Q 10.7.4.2). The long LeaveTime is safe only because of the
  immediate-leave rule: an explicit withdrawal is still seen at once.
  LeaveAllTime is drawn uniformly from [10 s, 15 s) per period, inside the
  9.5 s to 15.5 s tolerance (IEEE 802.1Q 10.7.4.3).
- **Changed values are declared New.** When an attribute we declare is
  re-declared with a different value (a talker moved to an adopted VID),
  the participant declares it New (IEEE 802.1Q-2018 10.7.5.1) so the change
  goes out at the next transmit opportunity rather than at the next periodic
  or LeaveAll.
- **Reserved table slots.** Without pruning, a bridge propagates every Talker
  Advertise on the network to an end station, and those registrations arrive
  before the station's own declarations (made after gPTP lock and MAAP).
  `MsrpConfig::reserved_*` keeps slots of each table for attributes that are
  ours: a Talker Advertise or Talker Failed we declare or whose stream one of
  our declared Listeners is bound to, and a Listener we declare or one for a
  stream we advertise. Foreign attributes are held to `max - reserved`. Size
  each reservation to the entity's own streams that use the table (Talker
  Advertise: talker streams plus listener streams).
- **Domain adoption.** `on_domain_declared` fires on every received Domain
  declaration (New, JoinIn, JoinMt, refreshes included) with the value as on
  the wire (SRclassID and SRclassPriority stepped per vector position,
  SRclassVID unchanged), even when the station declares the same SRclassID
  itself. An entity uses it to adopt the bridge's Class A parameters (Milan
  v1.2 Section 4.2.7.2.1).
- **Registered versus known advertises.** `find_registered_talker_advertise`
  returns a Talker Advertise only while a peer's declaration is registered
  (In or Leaving); `find_talker_advertise` also returns records the peer has
  left (kept until reclaimed) and ones only we declare.

## References

- IEEE 802.1Q-2014 Clause 10 (MRP), Clause 11.2 (MVRP), Clause 35 (MSRP)
- `mrp.c:446-545` — timer FSMs
- `mrp.c:551-904` — Applicant FSM
- `mrp.c:932-1054` — Registrar FSM
- `msrp.c:767-...` — MSRP event dispatch (for per-attribute driver
  pattern)
- `msrp.c:2002-2036` — sndmsg → wire AttributeEvent translation using
  paired Registrar state