<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# ATDECC Descriptor 2013/2021 Compatibility and the Zero-Copy Cast

## Background

IEEE 1722.1-2021 extended five AEM descriptor types with new tail fields
relative to 1722.1-2013:

| Descriptor | `MINIMUM_LENGTH` (2013) | `LENGTH` (2021) | Tail fields added in 2021 |
|---|---:|---:|---|
| `DescriptorStream` | 132 | 138 | `redundant_offset`, `number_of_redundant_streams`, `timing` |
| `DescriptorAvbInterface` | 98 | 102 | `number_of_controls`, `base_control` |
| `DescriptorAudioCluster` | 87 | 90 | `aes3_data_type_reference`, `aes3_data_type` |
| `DescriptorSignalTranscoder` | 92 | 100 | `transcoder_type` (`Eui64`) |
| `DescriptorControlBlock` | 76 | 82 | `signal_type`, `signal_index`, `signal_output` |

A compliant 1722.1-2021 implementation must accept `MINIMUM_LENGTH`-sized
descriptors from 1722.1-2013 peers.

## The Typing Gap

`parse_descriptor()` in `atdecc_aem_print.cpp` is a zero-copy overlay:

```cpp
auto const* parsed_descriptor = reinterpret_cast<ParsedDescriptor const*>(desc_data.data());
```

The returned pointer is a view over the caller's buffer. Once the code
holds a `DescriptorSignalTranscoder const&`, ordinary member access
(`d.transcoder_type`) reads the backing storage directly — but the
compiler has no idea whether that storage is `MINIMUM_LENGTH` or `LENGTH`
bytes long. The span's size was checked once against `MINIMUM_LENGTH` and
then thrown away.

This created an off-by-one heap-buffer-overflow class of bug: if the
parser accepts a 2013-compat short buffer and any downstream consumer
reads a tail-gap field, the read runs past the allocation. Caught by
AddressSanitizer in `atdecc_aem_print_test`, commit `61efc428`.

## Current Invariants

Two invariants must hold together. Neither is individually sufficient.

### Invariant 1: `MINIMUM_LENGTH` sits exactly at the 2013/2021 boundary

For each of the five affected structs, the offset of the first
2021-only field equals `MINIMUM_LENGTH`. Enforced by `static_assert`
next to the struct definition. Sample:

```cpp
static_assert(
    offsetof(DescriptorSignalTranscoder, transcoder_type) == DescriptorSignalTranscoder::MINIMUM_LENGTH,
    "transcoder_type is the first 2021-only field. Its offset must equal MINIMUM_LENGTH so "
    "format_to(descriptor, data_size) can gate the read on data_size >= LENGTH");
```

If someone reshuffles fields, reduces `MINIMUM_LENGTH`, or moves a 2013-era
field across the boundary, the assert fires at compile time.

### Invariant 2: consumers of tail-gap fields must gate on `data_size`

Formatters that render tail-gap fields must accept a `data_size`
parameter threaded from `format_descriptor()` and guard the read:

```cpp
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSignalTranscoder const& d,
               size_t data_size = DescriptorSignalTranscoder::LENGTH) -> OutputIt {
    // ... fields < MINIMUM_LENGTH always safe ...
    if (data_size >= DescriptorSignalTranscoder::LENGTH) {
        out = std::format_to(out, "          transcoder_type: ");
        out = ieee::format_to(out, d.transcoder_type);
        out = std::format_to(out, "\n");
    } else {
        out = std::format_to(out, "          transcoder_type: <none (pre-2021)>\n");
    }
    // ...
}
```

Today, only `format_to(DescriptorSignalTranscoder)` and
`format_to(DescriptorControlBlock)` read tail-gap fields by gating on
`data_size`. `AvbInterface` and `AudioCluster` have formatters that only
touch fields below `MINIMUM_LENGTH` — they are safe today but would need
the same treatment if future edits added reads of their 2021-only fields.

`DescriptorStream` is different: its 2021 fields sit *before* the
variable `stream_formats` trailer, not at the tail, so the 2013 and 2021
layouts put the formats at different offsets (132 vs 138). The
authoritative position is the descriptor's own `formats_offset`. Received
stream descriptors must go through `stream_descriptor_trailers(span)`,
which bounds-checks `formats_offset + 8 * number_of_formats` (and the
2021 `redundant_streams` list) against the wire length and only reads
`redundant_offset` / `number_of_redundant_streams` / `timing` when
`formats_offset >= LENGTH`. The inline `stream_formats` array and
`used_stream_formats()` describe the 2021 layout this repo serializes and
are only valid for descriptors it built itself.

The compiler cannot enforce invariant 2 — it's up to reviewers to
catch. The static_assert in invariant 1 at least makes the boundary
visible and load-bearing, so a developer adding a new 2021-only field
sees the pattern.

## Why not fix it globally?

Three architectural fixes would eliminate this class of bug instead of
guarding against it case-by-case. Each has a real cost.

### (a) `parse_descriptor` returns an owned, zero-padded copy

The `DescriptorAvbInterface` docstring already hints at this: *"Loaders
should use `span_load_padded` to safely ingest 2013-era payloads"*.
`span_load_padded` exists in the buffer module; `parse_descriptor`
doesn't use it.

If the parser returned a `DescriptorStorage` (owned, padded to
`LENGTH`), tail fields safely read zero for 2013-compat input and the
guard pattern disappears.

**Cost:** every `parse_descriptor()` call allocates. A controller
enumerating a large entity parses hundreds of descriptors at startup.
The zero-copy API was chosen deliberately for that workload.

### (b) Parser returns span + view together

```cpp
struct DescriptorSlice {
    std::span<uint8_t const> bytes;   // authoritative length
    ParsedDescriptor const* view;     // zero-copy overlay
};
```

Threads the length through every consumer. Still zero-copy.

**Cost:** every formatter, tool, and state machine that consumes a
parsed descriptor changes signature. Large API sweep.

### (c) Tail-gap fields become accessor methods, not struct members

```cpp
struct DescriptorSignalTranscoder {
    static constexpr size_t LENGTH = 100;
    static constexpr size_t MINIMUM_LENGTH = 92;
    // ... 2013-era fields as members ...
    [[nodiscard]] auto transcoder_type(size_t data_size) const -> std::optional<Eui64> {
        if (data_size < LENGTH) return std::nullopt;
        // read bytes at offset 92 via std::launder
        ...
    }
};
```

The caller physically cannot read the field without passing the length.
The memory layout is still zero-copy.

**Cost:** the accessor pattern is unusual in a project built around
plain-struct overlays; readability loss. And the zero-copy cast
(reinterpret/start_lifetime_as) still requires the full `LENGTH` of
addressable storage to be legal under the abstract machine — see below.

## Can C++23 utilities (`std::launder`, `std::start_lifetime_as`) help?

**`std::launder` (C++17):** addresses object-lifetime laundering after
in-place construction or memcpy over existing storage. It does not add
bounds information to a pointer; `std::launder(p)` returns a pointer of
the same type pointing at the same byte. It does not help here.

**`std::start_lifetime_as<T>` (C++23, P2590):** this is the *blessed*
replacement for `reinterpret_cast<T*>(bytes.data())` in zero-copy
overlay code. It implicitly starts the lifetime of a `T` in
implicit-lifetime-conforming storage, returning a usable `T*` without
calling a constructor. Once libc++ ships it (not yet implemented as of
LLVM 19), the parser's cast becomes:

```cpp
auto const* parsed = std::start_lifetime_as<ParsedDescriptor>(desc_data.data());
```

The **precondition is unchanged**: storage must be suitably aligned and
at least `sizeof(T)` bytes. Passing a 92-byte buffer when
`sizeof(DescriptorSignalTranscoder) == 100` is still UB — the function
codifies the precondition but does not enforce it.

So `start_lifetime_as` makes the cast well-defined (standard), but
doesn't prevent the short-buffer bug. It still requires a wrapper that
takes a span and checks length.

**`std::is_implicit_lifetime_v<T>` / `std::is_layout_compatible_v<T, U>`
(C++23 traits):** useful as `static_assert` on descriptor structs to
document the zero-copy overlay precondition. A candidate future
addition:

```cpp
static_assert(std::is_standard_layout_v<DescriptorSignalTranscoder>);
static_assert(std::is_trivially_copyable_v<DescriptorSignalTranscoder>);
// With C++26 when it lands:
// static_assert(std::is_implicit_lifetime_v<DescriptorSignalTranscoder>);
```

This catches accidental addition of a non-trivial member (virtual
function, non-trivial destructor, etc.) that would make the overlay UB
regardless of sizing.

**`std::bit_cast` (C++20):** copies, so not zero-copy. Not applicable.

## When to migrate to `start_lifetime_as`

Once libc++ ships `std::start_lifetime_as` (track LLVM release notes;
feature-test macro: `__cpp_lib_start_lifetime_as >= 202207L`), add a
thin helper in the buffer module:

```cpp
template <typename T>
constexpr auto span_view_as(std::span<uint8_t const> s) -> std::optional<T const*> {
    if (s.size() < sizeof(T)) return std::nullopt;
    return std::start_lifetime_as<T const>(s.data());
}
```

Gated behind `#if __cpp_lib_start_lifetime_as >= 202207L` with the
current `reinterpret_cast` as the fallback. Migrate `parse_descriptor`
to it once the compiler support is universal on supported platforms.

The `span_view_as` helper does not fix the 2013-compat issue (short
buffers are still short). Fix (c) — accessor methods — is the only way
to make tail-gap access statically safe while preserving zero-copy.

## Checklist: adding a new descriptor with a 2013-compat tail

1. Declare `static constexpr size_t MINIMUM_LENGTH` for the 2013-era size
   and `static constexpr size_t LENGTH` for the full 2021 size.
2. Place all 2013-era fields first, all 2021-only fields after. No
   interleaving.
3. Add a `static_assert(offsetof(struct, first_2021_field) == MINIMUM_LENGTH, "…")`
   pointing to this doc.
4. In the `format_to` overload (and any other zero-copy consumer):
   - Take `size_t data_size` as a parameter, defaulted to `LENGTH`.
   - Gate reads of any field at offset ≥ `MINIMUM_LENGTH` on
     `data_size >= LENGTH`.
5. Thread `data_size` through from `format_descriptor()` in
   `atdecc_aem_format.hpp`.