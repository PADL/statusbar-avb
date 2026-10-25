#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The AVB entity's persistent state: what controllers set that the entity
/// keeps across a restart (Milan v1.2 Section 5.3.4), in one TOML file keyed
/// by the entity model id. Schema and load rules: docs/PERSISTENT_STATE.md.

#include "statusbar/avb_entity/avb_entity_listener_bindings.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace statusbar::avb_entity {

struct PersistedName
{
    uint16_t descriptor_type{0};
    uint16_t descriptor_index{0};
    uint16_t name_index{0};
    std::string value;  ///< at most 64 octets (an ATDECC string), no NUL

    auto operator==(PersistedName const&) const -> bool = default;
};

struct PersistedStreamFormat
{
    uint16_t descriptor_type{0};
    uint16_t descriptor_index{0};
    uint64_t format{0};

    auto operator==(PersistedStreamFormat const&) const -> bool = default;
};

struct PersistedClockSource
{
    uint16_t clock_domain{0};
    uint16_t clock_source{0};

    auto operator==(PersistedClockSource const&) const -> bool = default;
};

struct PersistedPresentationOffset
{
    uint16_t stream_output{0};
    uint32_t ns{0};

    auto operator==(PersistedPresentationOffset const&) const -> bool = default;
};

/// A listener binding: the talker a stream input connects to and the input's
/// format when saved (0: unknown; not restored onto a different format), plus
/// a Milan input's binding parameters (Milan v1.2 Section 5.3.8.3, 5.3.8.7).
struct PersistedListenerBinding
{
    uint16_t stream_input{0};
    uint64_t talker_entity_id{0};
    uint16_t talker_unique_id{0};
    uint64_t format{0};
    uint64_t controller_entity_id{0};
    bool started{true};

    auto operator==(PersistedListenerBinding const&) const -> bool = default;
};

struct PersistedMediaClockReference
{
    uint16_t clock_domain{0};
    uint8_t user_priority{0};
    std::string domain_name;  ///< at most 64 octets, no NUL

    auto operator==(PersistedMediaClockReference const&) const -> bool = default;
};

/// One dynamic audio mapping (IEEE 1722.1 7.4.44).
struct PersistedAudioMapping
{
    uint16_t stream_index{0};
    uint16_t stream_channel{0};
    uint16_t cluster_offset{0};
    uint16_t cluster_channel{0};

    auto operator==(PersistedAudioMapping const&) const -> bool = default;
};

/// A stream port's dynamic mappings, all of them (possibly none).
struct PersistedAudioMap
{
    uint16_t descriptor_type{0};
    uint16_t descriptor_index{0};
    std::vector<PersistedAudioMapping> mappings;

    auto operator==(PersistedAudioMap const&) const -> bool = default;
};

struct PersistentState
{
    static constexpr int64_t VERSION = 1;

    uint64_t entity_model_id{0};
    /// The entity name the entity was configured with when this was written:
    /// a saved ENTITY name is dropped once the configuration names the entity
    /// differently (the configuration wins over stale state).
    std::string configured_entity_name;
    /// Likewise the presentation offset every talker stream was configured to
    /// start at: saved offsets are dropped once it changes.
    uint32_t configured_presentation_offset_ns{0};
    std::vector<PersistedName> names;
    std::vector<PersistedStreamFormat> stream_formats;
    std::vector<PersistedClockSource> clock_sources;
    std::vector<PersistedPresentationOffset> presentation_offsets;
    std::vector<PersistedListenerBinding> listener_bindings;
    uint32_t system_unique_id{0};
    std::vector<PersistedMediaClockReference> media_clock_references;
    std::vector<PersistedAudioMap> audio_maps;

    auto operator==(PersistentState const&) const -> bool = default;
};

/// The file's text for @p state (pure).
[[nodiscard]] auto serialize_persistent_state(PersistentState const& state) -> std::string;

/// Parse the file's text (pure). Fails with illegal_byte_sequence when it is
/// not the format (TOML errors, a missing or mistyped key, a value out of
/// range) and not_supported for another version.
[[nodiscard]] auto parse_persistent_state(std::string_view text) -> StatusValue<PersistentState>;

/// Write @p state to @p path atomically: a temporary file beside it, fsync,
/// rename over it, then fsync the directory. A missing directory (one level)
/// is created.
[[nodiscard]] auto save_persistent_state(std::string const& path, PersistentState const& state) -> Status;

/// What load_persistent_state found.
enum class PersistentStateOutcome : uint8_t
{
    fresh,          ///< no file (and no listener bindings file to import)
    loaded,         ///< the file, for this model
    imported,       ///< no file; the bindings of the old listener bindings file
    corrupt,        ///< not the format: moved to <path>.bad
    model_changed,  ///< written for another model id: discarded
    unreadable,     ///< present but could not be read (EACCES, EIO, EMFILE...): left alone
    newer_version,  ///< a later format version: left alone
};

struct LoadedPersistentState
{
    /// What to run with: the file's state, or a fresh one (only the bindings
    /// when imported). Its entity_model_id is always the running model's.
    PersistentState state;
    PersistentStateOutcome outcome{PersistentStateOutcome::fresh};
    /// corrupt, unreadable, newer_version: why (corrupt: bad_copy is the .bad path,
    /// empty when the move failed); model_changed: see discarded_model_id. After
    /// unreadable or newer_version the caller must not write the file this run.
    std::error_code error;
    std::string bad_copy;
    uint64_t discarded_model_id{0};
};

/// Load @p path for model @p model_id, importing @p legacy_bindings_path once
/// when @p path does not exist. A corrupt file is moved to <path>.bad; another
/// model's file is discarded; an unreadable or newer file is left alone.
[[nodiscard]] auto load_persistent_state(std::string const& path, uint64_t model_id, std::string const& legacy_bindings_path)
    -> LoadedPersistentState;

}  // namespace statusbar::avb_entity
