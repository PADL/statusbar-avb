// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_persistent_state.hpp"

#include "statusbar/toml/toml.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <format>
#include <limits>
#include <optional>
#include <utility>

#include <sys/stat.h>

namespace statusbar::avb_entity {

namespace {

//
// TOML text
//

/// A TOML basic string. Bytes from 0x80 go through as they are: ATDECC
/// strings are UTF-8, and the reader does not re-validate them.
auto toml_string(std::string_view const text) -> std::string
{
    std::string out{"\""};
    for (char const c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                    out += std::format("\\u{:04X}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

auto toml_hex(uint64_t const value) -> std::string
{
    return std::format("\"0x{:016x}\"", value);
}

//
// TOML values
//

using toml::Table;

/// An integer key in [min, max]; nullopt when absent, mistyped or out of range.
auto int_at(Table const& table, std::string_view const key, int64_t const min, int64_t const max) -> std::optional<int64_t>
{
    auto const* value = table.get(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    auto const n = value->as_integer();
    if (!n || *n < min || *n > max) {
        return std::nullopt;
    }
    return n;
}

auto u16_at(Table const& table, std::string_view const key) -> std::optional<uint16_t>
{
    if (auto const n = int_at(table, key, 0, std::numeric_limits<uint16_t>::max())) {
        return static_cast<uint16_t>(*n);
    }
    return std::nullopt;
}

auto u32_at(Table const& table, std::string_view const key) -> std::optional<uint32_t>
{
    if (auto const n = int_at(table, key, 0, std::numeric_limits<uint32_t>::max())) {
        return static_cast<uint32_t>(*n);
    }
    return std::nullopt;
}

/// A string key; nullopt when absent or mistyped
auto string_at(Table const& table, std::string_view const key) -> std::optional<std::string_view>
{
    auto const* value = table.get(key);
    return value != nullptr ? value->as_string() : std::nullopt;
}

/// A u64 in hex ("0x" optional); nullopt unless all of @p text is hex digits.
auto parse_hex(std::string_view text) -> std::optional<uint64_t>
{
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    uint64_t n = 0;
    auto const [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n, 16);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return n;
}

/// A u64 written as a hex string ("0x" optional)
auto hex_at(Table const& table, std::string_view const key) -> std::optional<uint64_t>
{
    auto const text = string_at(table, key);
    return text ? parse_hex(*text) : std::nullopt;
}

/// A boolean key; nullopt when absent or mistyped
auto bool_at(Table const& table, std::string_view const key) -> std::optional<bool>
{
    auto const* value = table.get(key);
    return value != nullptr ? value->as_boolean() : std::nullopt;
}

/// A string of at most 64 octets (an ATDECC string)
auto name_at(Table const& table, std::string_view const key) -> std::optional<std::string>
{
    auto const text = string_at(table, key);
    if (!text || text->size() > 64 || text->contains('\0')) {
        return std::nullopt;
    }
    return std::string{*text};
}

/// Each table of the array of tables @p key (absent: none) through @p load into
/// @p state; false when one is malformed.
auto each_table(Table const& root, std::string_view const key, PersistentState& state, bool (*load)(Table const&, PersistentState&))
    -> bool
{
    auto const* value = root.get(key);
    if (value == nullptr) {
        return true;
    }
    auto const* array = value->as_array();
    if (array == nullptr) {
        return false;
    }
    for (size_t i = 0; i < array->size(); ++i) {
        auto const* table = (*array)[i].as_table();
        if (table == nullptr || !load(*table, state)) {
            return false;
        }
    }
    return true;
}

auto malformed() -> std::error_code
{
    return std::make_error_code(std::errc::illegal_byte_sequence);
}

auto errno_code() -> std::error_code
{
    return {errno, std::generic_category()};
}

/// The rest of @p f; nullopt on a read error.
auto read_stream(std::FILE* const f) -> std::optional<std::string>
{
    std::string text;
    char buf[4096];
    size_t n = 0;
    // NOLINTNEXTLINE(clang-analyzer-unix.Stream) - the read after EOF returns 0 and ends the loop
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    if (std::ferror(f) != 0) {
        return std::nullopt;
    }
    return text;
}

/// Read the whole of @p path; nullopt with @p error set (ENOENT: absent).
auto read_file(std::string const& path, std::error_code& error) -> std::optional<std::string>
{
    std::FILE* const f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = errno_code();
        return std::nullopt;
    }
    auto text = read_stream(f);
    (void)std::fclose(f);
    if (!text) {
        error = std::make_error_code(std::errc::io_error);
    }
    return text;
}

/// The directory part of @p path ("." for a bare file name)
auto parent_of(std::string const& path) -> std::string
{
    auto const slash = path.rfind('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? std::string{"/"} : path.substr(0, slash);
}

auto write_all(int const fd, std::string_view text) -> bool
{
    while (!text.empty()) {
        auto const n = ::write(fd, text.data(), text.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        text.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

//
// Writing the file
//

/// The scalar keys, in file order
auto append_scalars(std::string& out, PersistentState const& state) -> void
{
    out += std::format("version = {}\n", PersistentState::VERSION);
    out += std::format("entity_model_id = {}\n", toml_hex(state.entity_model_id));
    out += std::format("configured_entity_name = {}\n", toml_string(state.configured_entity_name));
    out += std::format("configured_presentation_offset_ns = {}\n", state.configured_presentation_offset_ns);
    if (state.system_unique_id != 0) {
        out += std::format("system_unique_id = {}\n", state.system_unique_id);
    }
}

/// The [[name]], [[stream_format]], [[clock_source]] and [[presentation_offset]] tables
auto append_descriptor_tables(std::string& out, PersistentState const& state) -> void
{
    for (auto const& name : state.names) {
        out += std::format(
            "\n[[name]]\ndescriptor_type = {}\ndescriptor_index = {}\nname_index = {}\nvalue = {}\n",
            name.descriptor_type,
            name.descriptor_index,
            name.name_index,
            toml_string(name.value));
    }
    for (auto const& format : state.stream_formats) {
        out += std::format(
            "\n[[stream_format]]\ndescriptor_type = {}\ndescriptor_index = {}\nformat = {}\n",
            format.descriptor_type,
            format.descriptor_index,
            toml_hex(format.format));
    }
    for (auto const& source : state.clock_sources) {
        out += std::format("\n[[clock_source]]\nclock_domain = {}\nclock_source = {}\n", source.clock_domain, source.clock_source);
    }
    for (auto const& offset : state.presentation_offsets) {
        out += std::format("\n[[presentation_offset]]\nstream_output = {}\nns = {}\n", offset.stream_output, offset.ns);
    }
}

/// One [[listener_binding]] table; keys at their defaults are left out.
auto append_listener_binding(std::string& out, PersistedListenerBinding const& binding) -> void
{
    out += std::format(
        "\n[[listener_binding]]\nstream_input = {}\ntalker_entity_id = {}\ntalker_unique_id = {}\n",
        binding.stream_input,
        toml_hex(binding.talker_entity_id),
        binding.talker_unique_id);
    if (binding.format != 0) {
        out += std::format("format = {}\n", toml_hex(binding.format));
    }
    if (binding.controller_entity_id != 0) {
        out += std::format("controller_entity_id = {}\n", toml_hex(binding.controller_entity_id));
    }
    if (!binding.started) {
        out += "started = false\n";
    }
}

/// One [[audio_map]] table, its mappings inline
auto append_audio_map(std::string& out, PersistedAudioMap const& map) -> void
{
    out += std::format(
        "\n[[audio_map]]\ndescriptor_type = {}\ndescriptor_index = {}\nmappings = [", map.descriptor_type, map.descriptor_index);
    for (size_t i = 0; i < map.mappings.size(); ++i) {
        auto const& m = map.mappings[i];
        out += std::format(
            "{}[{}, {}, {}, {}]", i == 0 ? "" : ", ", m.stream_index, m.stream_channel, m.cluster_offset, m.cluster_channel);
    }
    out += "]\n";
}

/// The [[listener_binding]], [[media_clock_reference]] and [[audio_map]] tables
auto append_stream_tables(std::string& out, PersistentState const& state) -> void
{
    for (auto const& binding : state.listener_bindings) {
        append_listener_binding(out, binding);
    }
    for (auto const& mcr : state.media_clock_references) {
        out += std::format(
            "\n[[media_clock_reference]]\nclock_domain = {}\nuser_priority = {}\ndomain_name = {}\n",
            mcr.clock_domain,
            mcr.user_priority,
            toml_string(mcr.domain_name));
    }
    for (auto const& map : state.audio_maps) {
        append_audio_map(out, map);
    }
}

//
// Reading the file
//

/// Copy optional key @p key through @p get into @p out; false when present but malformed.
template <typename Get, typename T>
auto optional_into(Table const& table, std::string_view const key, Get const get, T& out) -> bool
{
    if (!table.contains(key)) {
        return true;
    }
    auto const value = get(table, key);
    if (!value) {
        return false;
    }
    out = *value;
    return true;
}

/// The file's version: not_supported unless it is this one.
auto check_version(Table const& root) -> Status
{
    auto const version = int_at(root, "version", 0, std::numeric_limits<int64_t>::max());
    if (!version) {
        return failure(malformed());
    }
    if (*version != PersistentState::VERSION) {
        return failure(std::make_error_code(std::errc::not_supported));
    }
    return success();
}

/// The entity model id and the optional scalar keys; false when one is malformed.
auto parse_scalars(Table const& root, PersistentState& state) -> bool
{
    auto const model_id = hex_at(root, "entity_model_id");
    if (!model_id) {
        return false;
    }
    state.entity_model_id = *model_id;
    return optional_into(root, "configured_entity_name", name_at, state.configured_entity_name) &&
        optional_into(root, "configured_presentation_offset_ns", u32_at, state.configured_presentation_offset_ns) &&
        optional_into(root, "system_unique_id", u32_at, state.system_unique_id);
}

struct DescriptorKey
{
    uint16_t type{0};
    uint16_t index{0};
};

/// A table's descriptor_type and descriptor_index
auto descriptor_at(Table const& t) -> std::optional<DescriptorKey>
{
    auto const type = u16_at(t, "descriptor_type");
    auto const index = u16_at(t, "descriptor_index");
    if (!type || !index) {
        return std::nullopt;
    }
    return DescriptorKey{.type = *type, .index = *index};
}

auto load_name(Table const& t, PersistentState& state) -> bool
{
    auto const key = descriptor_at(t);
    auto const name_index = u16_at(t, "name_index");
    auto value = name_at(t, "value");
    if (!key || !name_index || !value) {
        return false;
    }
    state.names.push_back(
        {.descriptor_type = key->type, .descriptor_index = key->index, .name_index = *name_index, .value = std::move(*value)});
    return true;
}

auto load_stream_format(Table const& t, PersistentState& state) -> bool
{
    auto const key = descriptor_at(t);
    auto const format = hex_at(t, "format");
    if (!key || !format) {
        return false;
    }
    state.stream_formats.push_back({.descriptor_type = key->type, .descriptor_index = key->index, .format = *format});
    return true;
}

auto load_clock_source(Table const& t, PersistentState& state) -> bool
{
    auto const domain = u16_at(t, "clock_domain");
    auto const source = u16_at(t, "clock_source");
    if (!domain || !source) {
        return false;
    }
    state.clock_sources.push_back({.clock_domain = *domain, .clock_source = *source});
    return true;
}

auto load_presentation_offset(Table const& t, PersistentState& state) -> bool
{
    auto const output = u16_at(t, "stream_output");
    auto const ns = u32_at(t, "ns");
    if (!output || !ns) {
        return false;
    }
    state.presentation_offsets.push_back({.stream_output = *output, .ns = *ns});
    return true;
}

/// A listener binding; the optional keys keep PersistedListenerBinding's defaults when absent.
auto load_listener_binding(Table const& t, PersistentState& state) -> bool
{
    auto const input = u16_at(t, "stream_input");
    auto const talker = hex_at(t, "talker_entity_id");
    auto const unique_id = u16_at(t, "talker_unique_id");
    if (!input || !talker || !unique_id) {
        return false;
    }
    auto& binding = state.listener_bindings.emplace_back(
        PersistedListenerBinding{.stream_input = *input, .talker_entity_id = *talker, .talker_unique_id = *unique_id});
    return optional_into(t, "format", hex_at, binding.format) &&
        optional_into(t, "controller_entity_id", hex_at, binding.controller_entity_id) &&
        optional_into(t, "started", bool_at, binding.started);
}

auto load_media_clock_reference(Table const& t, PersistentState& state) -> bool
{
    auto const domain = u16_at(t, "clock_domain");
    auto const prio = int_at(t, "user_priority", 0, 255);
    auto name = name_at(t, "domain_name");
    if (!domain || !prio || !name) {
        return false;
    }
    state.media_clock_references.push_back(
        {.clock_domain = *domain, .user_priority = static_cast<uint8_t>(*prio), .domain_name = std::move(*name)});
    return true;
}

/// One [stream_index, stream_channel, cluster_offset, cluster_channel] entry
auto mapping_at(toml::Value const& value) -> std::optional<PersistedAudioMapping>
{
    auto const* entry = value.as_array();
    if (entry == nullptr || entry->size() != 4) {
        return std::nullopt;
    }
    std::array<uint16_t, 4> fields{};
    for (size_t f = 0; f < fields.size(); ++f) {
        auto const n = (*entry)[f].as_integer();
        if (!n || *n < 0 || *n > std::numeric_limits<uint16_t>::max()) {
            return std::nullopt;
        }
        fields[f] = static_cast<uint16_t>(*n);
    }
    return PersistedAudioMapping{
        .stream_index = fields[0], .stream_channel = fields[1], .cluster_offset = fields[2], .cluster_channel = fields[3]};
}

/// The array @p value of mappings onto @p out; false when it is not one.
auto mappings_into(toml::Value const* const value, std::vector<PersistedAudioMapping>& out) -> bool
{
    auto const* list = value != nullptr ? value->as_array() : nullptr;
    if (list == nullptr) {
        return false;
    }
    for (size_t i = 0; i < list->size(); ++i) {
        auto const mapping = mapping_at((*list)[i]);
        if (!mapping) {
            return false;
        }
        out.push_back(*mapping);
    }
    return true;
}

auto load_audio_map(Table const& t, PersistentState& state) -> bool
{
    auto const key = descriptor_at(t);
    if (!key) {
        return false;
    }
    auto& map = state.audio_maps.emplace_back(PersistedAudioMap{.descriptor_type = key->type, .descriptor_index = key->index});
    return mappings_into(t.get("mappings"), map.mappings);
}

/// Every array of tables, in file order; false when a table is malformed.
auto parse_tables(Table const& root, PersistentState& state) -> bool
{
    return each_table(root, "name", state, load_name) && each_table(root, "stream_format", state, load_stream_format) &&
        each_table(root, "clock_source", state, load_clock_source) &&
        each_table(root, "presentation_offset", state, load_presentation_offset) &&
        each_table(root, "listener_binding", state, load_listener_binding) &&
        each_table(root, "audio_map", state, load_audio_map) &&
        each_table(root, "media_clock_reference", state, load_media_clock_reference);
}

//
// Saving and loading
//

/// Create @p tmp for writing, making @p path's directory when it is not there yet
/// (first start); -1 with errno set on failure.
auto open_for_write(std::string const& tmp, std::string const& path) -> int
{
    int const fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0 || errno != ENOENT) {
        return fd;
    }
    if (::mkdir(parent_of(path).c_str(), 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}

/// Write @p text to @p fd, fsync and close it; on failure @p tmp is removed.
auto write_and_close(int const fd, std::string const& tmp, std::string_view const text) -> Status
{
    bool const written = write_all(fd, text) && ::fsync(fd) == 0;
    auto const write_error = errno_code();
    bool const closed = ::close(fd) == 0;
    if (!written || !closed) {
        (void)::unlink(tmp.c_str());
        return failure(written ? errno_code() : write_error);
    }
    return success();
}

/// Rename @p tmp over @p path, then sync the directory so the rename survives a power cut.
auto replace_with(std::string const& tmp, std::string const& path) -> Status
{
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        auto const error = errno_code();
        (void)::unlink(tmp.c_str());
        return failure(error);
    }
    if (int const dir = ::open(parent_of(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir >= 0) {
        (void)::fsync(dir);
        (void)::close(dir);
    }
    return success();
}

/// The bindings of the old listener bindings file at @p path, once, into @p loaded
auto import_legacy_bindings(std::string const& path, LoadedPersistentState& loaded) -> void
{
    auto const bindings = load_listener_bindings(path);
    if (!bindings || bindings->empty()) {
        return;
    }
    for (auto const& b : *bindings) {
        loaded.state.listener_bindings.push_back(
            {.stream_input = b.listener_unique_id,
             .talker_entity_id = b.talker_entity_id.to_uint64(),
             .talker_unique_id = b.talker_unique_id});
    }
    loaded.outcome = PersistentStateOutcome::imported;
}

/// No file could be read: unreadable, or absent and perhaps imported.
auto load_unread(LoadedPersistentState loaded, std::error_code const read_error, std::string const& legacy_bindings_path)
    -> LoadedPersistentState
{
    if (read_error != std::errc::no_such_file_or_directory) {
        loaded.outcome = PersistentStateOutcome::unreadable;
        loaded.error = read_error;
        return loaded;
    }
    if (!legacy_bindings_path.empty()) {
        import_legacy_bindings(legacy_bindings_path, loaded);
    }
    return loaded;
}

/// The file at @p path did not parse: a newer version is left alone, anything else
/// is kept as <path>.bad for a post-mortem and the entity starts fresh.
auto load_rejected(LoadedPersistentState loaded, std::error_code const error, std::string const& path) -> LoadedPersistentState
{
    loaded.error = error;
    if (error == std::errc::not_supported) {
        loaded.outcome = PersistentStateOutcome::newer_version;
        return loaded;
    }
    loaded.outcome = PersistentStateOutcome::corrupt;
    std::string const bad = path + ".bad";
    if (std::rename(path.c_str(), bad.c_str()) == 0) {
        loaded.bad_copy = bad;
    }
    return loaded;
}

/// The file parsed: it is used when written for this model (loaded.state's model id).
auto load_parsed(LoadedPersistentState loaded, StatusValue<PersistentState> parsed, std::string const& path)
    -> LoadedPersistentState
{
    if (!parsed) {
        return load_rejected(std::move(loaded), parsed.error(), path);
    }
    if (parsed->entity_model_id != loaded.state.entity_model_id) {
        loaded.outcome = PersistentStateOutcome::model_changed;
        loaded.discarded_model_id = parsed->entity_model_id;
        return loaded;
    }
    loaded.state = std::move(*parsed);
    loaded.outcome = PersistentStateOutcome::loaded;
    return loaded;
}

}  // namespace

auto serialize_persistent_state(PersistentState const& state) -> std::string
{
    std::string out = "# The AVB entity's persistent state (Milan non-volatile settings), written by the\n"
                      "# entity whenever a controller changes one. Edit only while the entity is stopped.\n";
    append_scalars(out, state);
    append_descriptor_tables(out, state);
    append_stream_tables(out, state);
    return out;
}

auto parse_persistent_state(std::string_view const text) -> StatusValue<PersistentState>
{
    auto parsed = toml::Parser::parse(text);
    if (!parsed) {
        return failure(malformed());
    }
    if (auto const version = check_version(*parsed); !version) {
        return forward_failure(version);
    }
    PersistentState state{};
    if (!parse_scalars(*parsed, state) || !parse_tables(*parsed, state)) {
        return failure(malformed());
    }
    return success(std::move(state));
}

auto save_persistent_state(std::string const& path, PersistentState const& state) -> Status
{
    std::string const tmp = path + ".tmp";
    int const fd = open_for_write(tmp, path);
    if (fd < 0) {
        return failure(errno_code());
    }
    if (auto const written = write_and_close(fd, tmp, serialize_persistent_state(state)); !written) {
        return written;
    }
    return replace_with(tmp, path);
}

auto load_persistent_state(std::string const& path, uint64_t const model_id, std::string const& legacy_bindings_path)
    -> LoadedPersistentState
{
    LoadedPersistentState loaded{};
    loaded.state.entity_model_id = model_id;
    std::error_code read_error;
    auto const text = read_file(path, read_error);
    if (!text) {
        return load_unread(std::move(loaded), read_error, legacy_bindings_path);
    }
    return load_parsed(std::move(loaded), parse_persistent_state(*text), path);
}

}  // namespace statusbar::avb_entity
