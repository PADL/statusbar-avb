<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# AEM Entity Model

How the ATDECC AEM (AVB Entity Model) is represented, stored, and served in
this codebase.

## Context: what AEM is

**AEM** is the type system for ATDECC entities in IEEE 1722.1. It models an
AVB audio device as a **tree of typed descriptors** — a schema for "here's
what I am and what I can do." A controller asks the entity for its
descriptors (via `READ_DESCRIPTOR` AEM commands) and builds its own model of
the device from the responses.

The tree root is always a single **Entity descriptor**. Under it, one or
more **Configuration descriptors** (alternate personalities — "2-channel
mode" vs "8-channel mode", etc.). Under each configuration: audio units,
streams in/out, jacks in/out, AVB interfaces, clock sources/domains,
locales, strings, audio clusters, audio maps, controls, and a few more
types.

Each descriptor type has a **fixed wire format** (byte-for-byte layout
defined in IEEE 1722.1 Clause 7.2.x). The layout is what goes on the wire
when a controller does `READ_DESCRIPTOR`.

## The two storage mechanisms

This codebase has **two distinct** ways to hold descriptors, and it's easy
to confuse them:

### 1. `nanoavb::EntityModel` — typed, in-memory, runtime-constructed

Files:
- `statusbar/nanoavb/nanoavb_entity_model.hpp`
- `statusbar/nanoavb/nanoavb_entity_model.cpp`

This is the "I'm a C++ program that's going to ACT as an AEM entity at
runtime" path. You construct an `EntityModel` at startup, populate it with
descriptors via an `EntityModelBuilder`, and hand it to an `AemCommandHandler`
(the `READ_DESCRIPTOR` responder in `nanoavb_entity.hpp`).

The storage layout inside `EntityModel` is:

```cpp
DescriptorEntity entity_{};                                  // always exactly one

std::vector<DescriptorConfiguration> configurations_;
std::vector<DescriptorAudioUnit>     audio_units_;
std::vector<DescriptorStream>        stream_inputs_;
std::vector<DescriptorStream>        stream_outputs_;
std::vector<DescriptorJack>          jack_inputs_;
std::vector<DescriptorJack>          jack_outputs_;
std::vector<DescriptorAvbInterface>  avb_interfaces_;
std::vector<DescriptorClockSource>   clock_sources_;
std::vector<DescriptorClockDomain>   clock_domains_;
std::vector<DescriptorLocale>        locales_;
std::vector<DescriptorStrings>       strings_;
std::vector<DescriptorStreamPort>    stream_port_inputs_;
std::vector<DescriptorStreamPort>    stream_port_outputs_;
std::vector<DescriptorAudioCluster>  audio_clusters_;
std::vector<DescriptorAudioMap>      audio_maps_;
std::vector<DescriptorControl>       controls_;
```

One `std::vector` per descriptor type. Each `Descriptor*` is a
**trivially-copyable POD struct** with a statically-asserted byte layout
matching the IEEE wire format. For example:

```cpp
// statusbar/atdecc/atdecc_aem_descriptor.hpp
struct DescriptorEntity
{
    static constexpr size_t LENGTH = 312;

    doublet_t descriptor_type{DESCRIPTOR_ENTITY};  // offset 0
    doublet_t descriptor_index{0};                 // offset 2
    Eui64 entity_id{};                             // offset 4
    Eui64 entity_model_id{};                       // offset 12
    quadlet_t entity_capabilities{0};              // offset 20
    // ... more fields ...
    doublet_t configurations_count{0};             // offset 308
    doublet_t current_configuration{0};            // offset 310
};
static_assert(sizeof(DescriptorEntity) == 312, "DescriptorEntity must be exactly 312 bytes");
static_assert(offsetof(DescriptorEntity, descriptor_type) == 0);
// ... many more offsetof checks
```

So the **in-memory C++ struct layout is identical to the wire format**.
This means a `READ_DESCRIPTOR` response doesn't need to serialize anything —
it just takes a byte-view of the struct and copies it into the response
body. That's what `get_descriptor_raw` does:

```cpp
case DESCRIPTOR_ENTITY:
    if (descriptor_index != 0) {
        return failure(make_error_code(NanoAvbError::InvalidDescriptorIndex));
    }
    return make_const_span(entity_);  // view the struct as bytes, return

case DESCRIPTOR_CONFIGURATION:
    if (descriptor_index >= configurations_.size()) {
        return failure(make_error_code(NanoAvbError::InvalidDescriptorIndex));
    }
    return make_const_span(configurations_[descriptor_index]);
// ... 14 more cases
```

`make_const_span(T const&)` is the trivially-copyable overload from
`statusbar/buffer/span_utils.hpp` — it `reinterpret_cast`s the struct into a
`std::span<uint8_t const, sizeof(T)>`. Zero copy. The descriptor's wire
bytes ARE its memory bytes.

### 2. `atdecc::aem::DescriptorStorage` — a zero-copy reader for a pre-built binary blob

File: `statusbar/atdecc/atdecc_descriptor_storage.hpp`.

This is a completely different mechanism: it reads a **`.aem` binary file
format** that contains a pre-built descriptor table:

```
[20-byte header: magic 'AEM1' + TOC offset/count + symbol-table offset/count]
[TOC: N × 12-byte (type, index, config, length, offset) records]
[Symbol table: M × 10-byte (type, index, config, symbol) records]
[Raw descriptor bytes at offsets pointed to by TOC entries]
```

This format comes from `jdksavdecc` (per the comment at the top of the
file). It's meant for vendor tools that author entity models offline — a
manufacturer builds their device's AEM tree in a tool, exports a `.aem`
blob, and the device firmware bakes that blob in and serves descriptors out
of it at runtime.

`DescriptorStorage::get_descriptor(config, type, index)` linearly scans the
TOC looking for a match, then returns a `std::span<uint8_t const>` pointing
directly into the blob. Zero copy, no C++ structs.

The symbol table is metadata for the kit, never served to a controller
(READ_DESCRIPTOR serves TOC entries only). Besides the authored symbols
(`symbol` in the JSON/XML models), the ENTITY descriptor may carry one
well-known symbol, `entity_profile:milan` or `entity_profile:classic`,
compiled from the model's entity `milan` attribute: the entity's ATDECC
profile (`nanoavb::EntityProfile`, `nanoavb_entity_profile.hpp`). Without
it the kit runs Milan; the kit config's `milan` overrides it.

### How the two interact

`EntityModel` has an optional external `DescriptorStorage`:

```cpp
/// Set an external DescriptorStorage blob for READ_DESCRIPTOR fallback.
/// When set, get_descriptor_raw() checks the blob first, falling back
/// to the internal vectors only for descriptor types not found in storage.
/// The blob must outlive the EntityModel.
void set_descriptor_storage(atdecc::aem::DescriptorStorage storage);
```

And the `get_descriptor_raw` dispatch does:

```cpp
// Check external DescriptorStorage blob first (if attached)
if (descriptor_storage_.has_value()) {
    auto result = descriptor_storage_->get_descriptor(0, descriptor_type, descriptor_index);
    if (result.has_value()) {
        return result;
    }
    // Fall through to internal vectors if not found in storage
}

switch (descriptor_type) { ... }
```

So the design is: **the static blob is authoritative; the runtime vectors
cover anything the blob doesn't have.** In practice,
`avb_entity_am824_io.cpp` and `avb_entity_stereo_io.cpp` both load a blob
and THEN call `EntityModelBuilder` to add a small set of additional
descriptors that weren't in the blob or that need runtime patching.

## How it's constructed at startup

`EntityModelBuilder` is a fluent builder that wraps an `EntityModel`:

```cpp
auto model = EntityModelBuilder{}
    .entity(DescriptorEntity{
        .entity_id = Eui64{...},
        .entity_name = AtdeccString{"My AVB Box"},
        .configurations_count = 1,
        // ...
    })
    .configuration(DescriptorConfiguration{
        .object_name = AtdeccString{"Default"},
        // ...
    })
    .stream_input(DescriptorStream{ /* ... */ })
    .stream_input(DescriptorStream{ /* ... */ })
    .clock_domain(DescriptorClockDomain{ /* ... */ })
    .clock_source(DescriptorClockSource{ /* ... */ })
    .build();
```

Each builder method does `model_.add_<type>(desc)` and returns `*this`. The
`add_*` methods push onto the internal vector and auto-assign
`descriptor_index` (and sometimes `descriptor_type`, for types where the
same struct represents two roles — e.g., `DescriptorStream` is used for
both `DESCRIPTOR_STREAM_INPUT` and `DESCRIPTOR_STREAM_OUTPUT`).

`build()` moves the `EntityModel` out of the builder, so the builder is
single-use.

At construction time, `EntityModel` calls `reserve_storage()` which calls
`.reserve(config_.max_*)` on every vector. This is a one-time heap
allocation at startup, zero allocations after. The configurable
`EntityModelConfig` struct holds the capacity knobs.

## How `READ_DESCRIPTOR` flows through the model at runtime

This is the full chain that happens when a controller reads a descriptor:

1. A controller sends `READ_DESCRIPTOR(config_index, descriptor_type, descriptor_index)` to the entity.
2. The entity's `AemCommandHandler::process_packet`
   (`statusbar/nanoavb/nanoavb_entity.cpp`) parses the AEM header, verifies
   the target matches, and dispatches to
   `handle_read_descriptor(header, command_data, out_buffer)`.
3. `handle_read_descriptor` parses the command body (8 bytes: config_index,
   reserved, descriptor_type, descriptor_index), validates the config_index,
   and calls `model_.get_descriptor_raw(descriptor_type, descriptor_index)`.
4. `get_descriptor_raw` either:
   - Finds it in the attached `DescriptorStorage` blob (and returns a span
     into the blob), or
   - Hits one of the 17 switch cases for the in-memory `EntityModel` vectors
     (and returns `make_const_span(<vector>[index])`), or
   - Returns `NanoAvbError::InvalidDescriptorIndex` / `InvalidDescriptorType`.
5. `handle_read_descriptor` writes the echoed command header (8 bytes: config
   + reserved + type + index) into the output buffer, then appends the
   descriptor bytes, and returns `{status: SUCCESS, size: 8 + descriptor_size}`.
6. `process_packet` hands the populated buffer to `send_response`, which
   wraps it in an `AemDu` response header and ships it via the
   `send_response` callback.

## Observations and known gaps

### 1. The `get_descriptor_raw` switch is ~100 lines of repetition

`nanoavb_entity_model.cpp` has 17 nearly-identical switch cases:

```cpp
case DESCRIPTOR_CONFIGURATION:
    if (descriptor_index >= configurations_.size()) {
        return failure(make_error_code(NanoAvbError::InvalidDescriptorIndex));
    }
    return make_const_span(configurations_[descriptor_index]);

case DESCRIPTOR_AUDIO_UNIT:
    if (descriptor_index >= audio_units_.size()) {
        return failure(make_error_code(NanoAvbError::InvalidDescriptorIndex));
    }
    return make_const_span(audio_units_[descriptor_index]);

// ... 14 more cases of exactly this shape
```

Same for the 17 `add_*` and `get_*` wrappers (the `.hpp` has 17 `size()`
accessors, 17 typed `get_*`, 17 typed `add_*`, and then the switch-based
raw dispatch in the `.cpp`). That's ~70 functions whose bodies are one-liners
into the right `std::vector`.

A table-driven approach could collapse this, but the vectors have different
element types, so you'd need a type-erasing wrapper per entry. The current
shape is genuinely verbose but boringly correct — not obviously worth
changing unless a new descriptor type has to be added (and then only ~5
lines touch).

A more interesting simplification would be to drop the per-type
`get_*`/`add_*` methods from the public API and expose only
`get_descriptor_raw` / `add_descriptor`. Callers would work exclusively in
terms of the wire format. That's a bigger ideological shift — the current
API is friendly to "build this entity in C++" code.

### 2. The in-memory model can't represent `DescriptorConfiguration`'s `descriptor_counts` table correctly

`DescriptorConfiguration::LENGTH = 74` and the struct has:

```cpp
struct DescriptorConfiguration {
    // ...
    doublet_t descriptor_counts_count{0};
    doublet_t descriptor_counts_offset{0};
};
```

The `descriptor_counts_offset` field points to a **variable-length trailer**
in the wire format containing `(descriptor_type, count)` pairs — one per
child descriptor type in this configuration. The C++ struct is 74 bytes and
the trailer lives OUTSIDE the struct, somewhere beyond it.

In `DescriptorStorage`'s binary blob format, the trailer is present in the
blob bytes at the offset the TOC says. `get_descriptor()` returns a span of
`length` bytes starting at `offset` — the length CAN be greater than 74
bytes so the trailer is included.

But in `EntityModel`'s in-memory `std::vector<DescriptorConfiguration>`,
each element is **exactly** `sizeof(DescriptorConfiguration) == 74` bytes,
because that's all `std::vector<T>` stores. `make_const_span(configurations_[i])`
gives you 74 bytes — the trailer is **missing**. If a controller reads a
CONFIGURATION descriptor from an entity built via `EntityModelBuilder`, it
gets a truncated response.

The typical flow in `avb_entity_am824_io.cpp` and friends is to load a
pre-built blob via `DescriptorStorage`, so the blob has the full trailer
and the fallback path doesn't get hit for CONFIGURATION. But an entity
built ENTIRELY via `EntityModelBuilder` (like the ones in
`nanoavb_entity_test.cpp`) has no trailer, and `handle_read_descriptor`
would return a 74-byte body where the wire format requires `74 + 4*N` bytes.

This is a real incompleteness. It's not currently biting anyone because
every real-world entity path in this codebase uses the blob. But the
abstraction is leaky and the builder tests can't exercise the full wire
format.

### 3. Only a subset of AEM descriptor types are supported in `EntityModel`

`atdecc_aem_descriptor.hpp` defines **27 descriptor types**:
DescriptorEntity, Configuration, AudioUnit, VideoUnit, SensorUnit, Stream,
Jack, AvbInterface, ClockSource, MemoryObject, Locale, Strings, StreamPort,
ExternalPort, InternalPort, AudioCluster, VideoCluster, SensorCluster,
AudioMap, VideoMap, SensorMap, Control, SignalSelector, Mixer, Matrix,
MatrixSignal, SignalSplitter.

`EntityModel` has `add_*` and `get_*` methods for only **16 of them**.
Missing from the builder API:

- Video (VideoUnit, VideoCluster, VideoMap)
- Sensor (SensorUnit, SensorCluster, SensorMap)
- MemoryObject
- ExternalPort, InternalPort
- SignalSelector, Mixer, Matrix, MatrixSignal, SignalSplitter

The fallback path to `DescriptorStorage` WILL serve these if they're in the
blob (because `get_descriptor` just looks up by `(type, index)`), but you
can't build one from code via `EntityModelBuilder`. For a library targeting
audio AVB devices, the video/sensor omissions are fine. The MemoryObject /
mixer / matrix omissions are more meaningful — those are used by real audio
products. If someone wanted to write a native (blob-free) mixer entity,
they'd be blocked.

### 4. `EntityModelBuilder::build()` leaves the builder in a moved-from state with no defense

```cpp
[[nodiscard]] auto build() { return std::move(model_); }
```

After `build()`, `model_` is moved-from. Calling `build()` again returns a
default-constructed/empty `EntityModel`. Calling any of the
`.stream_input(...)` methods after `build()` operates on the moved-from
vector (which is valid but empty — `push_back` would work but allocate
freshly, throwing off the reserved capacity).

Typical builder pattern. Not broken, just not defensively checked. A debug
`assert(!built_)` would catch misuse.

### 5. `EntityModel` uses `std::vector` with `reserve` — deliberately different from the inflight `SlotTable` pattern

Each vector gets a `.reserve(max_<type>)` call in `reserve_storage()`. This
is the same "Pattern B" shape we replaced with `SlotTable` for the
controller's inflight tracking.

**But here it's the right choice**, unlike the controller case. The entity
model does `add_*` at startup and `get_*` at runtime — it never does
`remove_*`. No swap-with-last, no slot reuse, no "table full" silent drop.
The heap allocation happens once at construction, and after that it's a
plain indexed-read of PODs. `SlotTable<T, N>` would give no real benefit:
no find_if pattern in the hot path, no runtime add/remove, no
"never heap-allocate" invariant since the one-time reserve() is fine.

If the style consistency is important, this is where `SlotTable` would go
next. The decision is a value judgment: consistency vs. not-fixing-what-
isn't-broken.

### 6. Thread model is not documented

Same observation that applies to `AemCommandHandler`: the `EntityModel`
class has no thread-model doc. In practice, `EntityModel` is constructed
once at startup, then `get_descriptor_raw` is called from the AEM state-
machine thread. Read-only access after construction is thread-safe by
default. But that's not documented.

One subtle concern: `EntityModel` has mutating accessors (`get_entity_mut()`,
`set_entity()`, `add_*`). If any runtime code calls these concurrently with
`get_descriptor_raw` from another thread (e.g., a management API that
updates entity state while the reactor serves `READ_DESCRIPTOR`), there's a
data race. The class doesn't defend against this and doesn't document the
expectation. Low priority but worth a class-level comment.

### 7. Positive: the wire-format-as-C++-struct trick is clever

The design choice that `Descriptor*` structs are trivially-copyable PODs
with the wire layout baked into their member offsets is really elegant. It
means:

- Zero serialization work at runtime — `make_const_span(entity_)` is a
  `reinterpret_cast`.
- No separate parser/serializer code paths — the struct IS the parser.
- Static verification via `static_assert(sizeof(T) == LENGTH)` +
  per-field `offsetof` checks catches accidental padding or field-order
  drift at compile time.
- The same trick used throughout `span_utils.hpp` (`span_load` /
  `span_store` / `span_pack_header_payload`) pairs naturally with these
  structs.

The cost is endianness: multi-byte fields have to be wrapped in
`doublet_t` / `quadlet_t` / `Eui64` byte-swap types so the in-memory
representation matches network byte order. The cost is worth it for the
ergonomics.

## Recommendations (priority order)

1. **Fix the `DescriptorConfiguration` trailer problem** — either drop
   `EntityModel`-built entities' ability to serve CONFIGURATION descriptors
   without a backing blob, or extend the model to store the `(type, count)`
   child pairs alongside each configuration so `make_const_span` + a
   trailer append can produce a valid wire response. This is the only
   finding that could bite a user building an entity from C++ alone.

2. **Add a thread-model class-level doc** to both `EntityModel` and
   `EntityModelBuilder`. Matches the doc added to `AemCommandHandler`.

3. **Consider adding missing descriptor types** (MemoryObject, Mixer,
   Matrix, SignalSelector) to the builder API — only if there's a near-term
   use case. For pure audio AVB boxes targeting AM824 / AAF streams, the
   current set is enough.

4. **Leave the `get_descriptor_raw` switch alone.** It looks verbose but
   it's straightforward, clang-tidy-clean, and has static per-case bounds
   checks. A cleverer dispatch trades readability for brevity in a way that
   doesn't pay off.

5. **Leave the `std::vector` storage alone.** `SlotTable` migration would
   be consistency-for-its-own-sake — the usage pattern genuinely differs
   (add-at-startup, read-at-runtime, never remove).

## See also

- `docs/NANOAVB_ARCHITECTURE.md` — overall NanoAVB module design
- `docs/DESERIALIZER_GUIDE.md` — the `CompiledDeserializer` pattern used by
  related wire-format parsers
- `statusbar/atdecc/atdecc_aem_descriptor.hpp` — full descriptor struct
  definitions
- `statusbar/atdecc/atdecc_descriptor_storage.hpp` — binary blob format
- `statusbar/nanoavb/nanoavb_entity.hpp` — `AemCommandHandler` (the
  `READ_DESCRIPTOR` responder)
- `statusbar/nanoavb/nanoavb_entity_model.hpp` — `EntityModel` and
  `EntityModelBuilder`
