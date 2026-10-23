// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_persistent_state.hpp"

#include "statusbar/toml/toml.hpp"

#include <fcntl.h>
#include <unistd.h>

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

// ---- writing ----

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

// ---- reading ----

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

/// A u64 written as a hex string ("0x" optional)
auto hex_at(Table const& table, std::string_view const key) -> std::optional<uint64_t>
{
    auto const* value = table.get(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    auto text = value->as_string();
    if (!text) {
        return std::nullopt;
    }
    if (text->starts_with("0x") || text->starts_with("0X")) {
        text->remove_prefix(2);
    }
    uint64_t n = 0;
    auto const [end, ec] = std::from_chars(text->data(), text->data() + text->size(), n, 16);
    if (ec != std::errc{} || end != text->data() + text->size() || text->empty()) {
        return std::nullopt;
    }
    return n;
}

/// A string of at most 64 octets (an ATDECC string)
auto name_at(Table const& table, std::string_view const key) -> std::optional<std::string>
{
    auto const* value = table.get(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    auto const text = value->as_string();
    if (!text || text->size() > 64 || text->contains('\0')) {
        return std::nullopt;
    }
    return std::string{*text};
}

/// Each table of the array of tables @p key (absent: none) through @p fn,
/// which returns false for a malformed one.
template <typename Fn>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) - fn is called, not forwarded
auto each_table(Table const& root, std::string_view const key, Fn&& fn) -> bool
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
        if (table == nullptr || !fn(*table)) {
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

/// Read the whole of @p path; nullopt with @p error set (ENOENT: absent).
auto read_file(std::string const& path, std::error_code& error) -> std::optional<std::string>
{
    std::FILE* const f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = errno_code();
        return std::nullopt;
    }
    std::string text;
    char buf[4096];
    size_t n = 0;
    // NOLINTNEXTLINE(clang-analyzer-unix.Stream) - the read after EOF returns 0 and ends the loop
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    bool const failed = std::ferror(f) != 0;
    (void)std::fclose(f);
    if (failed) {
        error = std::make_error_code(std::errc::io_error);
        return std::nullopt;
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

}  // namespace

auto serialize_persistent_state(PersistentState const& state) -> std::string
{
    std::string out =
        "# The AVB entity's persistent state (Milan non-volatile settings), written by the\n"
        "# entity whenever a controller changes one. Edit only while the entity is stopped.\n";
    out += std::format("version = {}\n", PersistentState::VERSION);
    out += std::format("entity_model_id = {}\n", toml_hex(state.entity_model_id));
    out += std::format("configured_entity_name = {}\n", toml_string(state.configured_entity_name));
    out += std::format("configured_presentation_offset_ns = {}\n", state.configured_presentation_offset_ns);
    if (state.system_unique_id != 0) {
        out += std::format("system_unique_id = {}\n", state.system_unique_id);
    }
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
    for (auto const& rate : state.sampling_rates) {
        out += std::format("\n[[sampling_rate]]\naudio_unit = {}\nrate = {}\n", rate.audio_unit, rate.rate);
    }
    for (auto const& source : state.clock_sources) {
        out += std::format("\n[[clock_source]]\nclock_domain = {}\nclock_source = {}\n", source.clock_domain, source.clock_source);
    }
    for (auto const& offset : state.presentation_offsets) {
        out += std::format("\n[[presentation_offset]]\nstream_output = {}\nns = {}\n", offset.stream_output, offset.ns);
    }
    for (auto const& binding : state.listener_bindings) {
        out += std::format(
            "\n[[listener_binding]]\nstream_input = {}\ntalker_entity_id = {}\ntalker_unique_id = {}\n",
            binding.stream_input,
            toml_hex(binding.talker_entity_id),
            binding.talker_unique_id);
        if (binding.format != 0) {
            out += std::format("format = {}\n", toml_hex(binding.format));
        }
    }
    for (auto const& mcr : state.media_clock_references) {
        out += std::format(
            "\n[[media_clock_reference]]\nclock_domain = {}\nuser_priority = {}\ndomain_name = {}\n",
            mcr.clock_domain,
            mcr.user_priority,
            toml_string(mcr.domain_name));
    }
    return out;
}

auto parse_persistent_state(std::string_view const text) -> StatusValue<PersistentState>
{
    auto parsed = toml::Parser::parse(text);
    if (!parsed) {
        return failure(malformed());
    }
    Table const& root = *parsed;
    auto const version = int_at(root, "version", 0, std::numeric_limits<int64_t>::max());
    if (!version) {
        return failure(malformed());
    }
    if (*version != PersistentState::VERSION) {
        return failure(std::make_error_code(std::errc::not_supported));
    }
    PersistentState state{};
    auto const model_id = hex_at(root, "entity_model_id");
    if (!model_id) {
        return failure(malformed());
    }
    state.entity_model_id = *model_id;
    if (root.contains("configured_entity_name")) {
        auto const configured = name_at(root, "configured_entity_name");
        if (!configured) {
            return failure(malformed());
        }
        state.configured_entity_name = *configured;
    }
    if (root.contains("configured_presentation_offset_ns")) {
        auto const offset = u32_at(root, "configured_presentation_offset_ns");
        if (!offset) {
            return failure(malformed());
        }
        state.configured_presentation_offset_ns = *offset;
    }
    if (root.contains("system_unique_id")) {
        auto const id = u32_at(root, "system_unique_id");
        if (!id) {
            return failure(malformed());
        }
        state.system_unique_id = *id;
    }

    bool const ok =
        each_table(
            root,
            "name",
            [&](Table const& t) {
                auto const type = u16_at(t, "descriptor_type");
                auto const index = u16_at(t, "descriptor_index");
                auto const name_index = u16_at(t, "name_index");
                auto value = name_at(t, "value");
                if (!type || !index || !name_index || !value) {
                    return false;
                }
                state.names.push_back(
                    {.descriptor_type = *type, .descriptor_index = *index, .name_index = *name_index, .value = std::move(*value)});
                return true;
            }) &&
        each_table(
            root,
            "stream_format",
            [&](Table const& t) {
                auto const type = u16_at(t, "descriptor_type");
                auto const index = u16_at(t, "descriptor_index");
                auto const format = hex_at(t, "format");
                if (!type || !index || !format) {
                    return false;
                }
                state.stream_formats.push_back({.descriptor_type = *type, .descriptor_index = *index, .format = *format});
                return true;
            }) &&
        each_table(
            root,
            "sampling_rate",
            [&](Table const& t) {
                auto const unit = u16_at(t, "audio_unit");
                auto const rate = u32_at(t, "rate");
                if (!unit || !rate) {
                    return false;
                }
                state.sampling_rates.push_back({.audio_unit = *unit, .rate = *rate});
                return true;
            }) &&
        each_table(
            root,
            "clock_source",
            [&](Table const& t) {
                auto const domain = u16_at(t, "clock_domain");
                auto const source = u16_at(t, "clock_source");
                if (!domain || !source) {
                    return false;
                }
                state.clock_sources.push_back({.clock_domain = *domain, .clock_source = *source});
                return true;
            }) &&
        each_table(
            root,
            "presentation_offset",
            [&](Table const& t) {
                auto const output = u16_at(t, "stream_output");
                auto const ns = u32_at(t, "ns");
                if (!output || !ns) {
                    return false;
                }
                state.presentation_offsets.push_back({.stream_output = *output, .ns = *ns});
                return true;
            }) &&
        each_table(
            root,
            "listener_binding",
            [&](Table const& t) {
                auto const input = u16_at(t, "stream_input");
                auto const talker = hex_at(t, "talker_entity_id");
                auto const unique_id = u16_at(t, "talker_unique_id");
                auto const format = t.contains("format") ? hex_at(t, "format") : std::optional<uint64_t>{0};
                if (!input || !talker || !unique_id || !format) {
                    return false;
                }
                state.listener_bindings.push_back(
                    {.stream_input = *input, .talker_entity_id = *talker, .talker_unique_id = *unique_id, .format = *format});
                return true;
            }) &&
        each_table(root, "media_clock_reference", [&](Table const& t) {
            auto const domain = u16_at(t, "clock_domain");
            auto const prio = int_at(t, "user_priority", 0, 255);
            auto name = name_at(t, "domain_name");
            if (!domain || !prio || !name) {
                return false;
            }
            state.media_clock_references.push_back(
                {.clock_domain = *domain, .user_priority = static_cast<uint8_t>(*prio), .domain_name = std::move(*name)});
            return true;
        });
    if (!ok) {
        return failure(malformed());
    }
    return success(std::move(state));
}

auto save_persistent_state(std::string const& path, PersistentState const& state) -> Status
{
    std::string const tmp = path + ".tmp";
    auto const text = serialize_persistent_state(state);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 && errno == ENOENT) {
        // The state directory is not there yet (first start): make it.
        if (::mkdir(parent_of(path).c_str(), 0755) != 0 && errno != EEXIST) {
            return failure(errno_code());
        }
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        return failure(errno_code());
    }
    bool const written = write_all(fd, text) && ::fsync(fd) == 0;
    auto const write_error = errno_code();
    bool const closed = ::close(fd) == 0;
    if (!written || !closed) {
        (void)::unlink(tmp.c_str());
        return failure(written ? errno_code() : write_error);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        auto const error = errno_code();
        (void)::unlink(tmp.c_str());
        return failure(error);
    }
    // The rename itself survives a power cut once the directory is synced.
    if (int const dir = ::open(parent_of(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir >= 0) {
        (void)::fsync(dir);
        (void)::close(dir);
    }
    return success();
}

auto load_persistent_state(std::string const& path, uint64_t const model_id, std::string const& legacy_bindings_path)
    -> LoadedPersistentState
{
    LoadedPersistentState loaded{};
    loaded.state.entity_model_id = model_id;
    std::error_code read_error;
    auto const text = read_file(path, read_error);
    if (!text) {
        if (read_error != std::errc::no_such_file_or_directory) {
            loaded.outcome = PersistentStateOutcome::unreadable;
            loaded.error = read_error;
            return loaded;
        }
        // No state yet: take the old listener bindings file's bindings, once.
        if (!legacy_bindings_path.empty()) {
            if (auto bindings = load_listener_bindings(legacy_bindings_path); bindings && !bindings->empty()) {
                for (auto const& b : *bindings) {
                    loaded.state.listener_bindings.push_back(
                        {.stream_input = b.listener_unique_id,
                         .talker_entity_id = b.talker_entity_id.to_uint64(),
                         .talker_unique_id = b.talker_unique_id});
                }
                loaded.outcome = PersistentStateOutcome::imported;
            }
        }
        return loaded;
    }
    auto parsed = parse_persistent_state(*text);
    if (!parsed && parsed.error() == std::errc::not_supported) {
        loaded.outcome = PersistentStateOutcome::newer_version;
        loaded.error = parsed.error();
        return loaded;
    }
    if (!parsed) {
        loaded.outcome = PersistentStateOutcome::corrupt;
        loaded.error = parsed.error();
        // Keep it for a post-mortem; the entity starts fresh and writes anew.
        std::string const bad = path + ".bad";
        if (std::rename(path.c_str(), bad.c_str()) == 0) {
            loaded.bad_copy = bad;
        }
        return loaded;
    }
    if (parsed->entity_model_id != model_id) {
        loaded.outcome = PersistentStateOutcome::model_changed;
        loaded.discarded_model_id = parsed->entity_model_id;
        return loaded;
    }
    loaded.state = std::move(*parsed);
    loaded.outcome = PersistentStateOutcome::loaded;
    return loaded;
}

}  // namespace statusbar::avb_entity
