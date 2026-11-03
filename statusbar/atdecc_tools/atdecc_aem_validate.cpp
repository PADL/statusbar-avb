// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc_tools/atdecc_aem_validate.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <algorithm>
#include <format>
#include <functional>

namespace statusbar::atdecc_tools {

using namespace statusbar::atdecc::aem;

namespace {

[[nodiscard]] auto find_desc(std::vector<RawDescriptor> const& d, uint16_t type, uint16_t index) -> RawDescriptor const*
{
    for (auto const& r : d) {
        if (r.type == type && r.index == index) {
            return &r;
        }
    }
    return nullptr;
}

[[nodiscard]] auto count_type(std::vector<RawDescriptor> const& d, uint16_t type) -> size_t
{
    return static_cast<size_t>(std::count_if(d.begin(), d.end(), [&](auto const& r) { return r.type == type; }));
}

/// Takes one finding: its severity, where it is and what it is.
using Add = std::function<void(Severity, std::string, std::string)>;

auto validate_aem_model(std::vector<RawDescriptor> const& descs) -> std::vector<ModelFinding>
{  // NOLINT(statusbar-block-statements) the model checks, one per descriptor kind; pre-dates the gate
    std::vector<ModelFinding> out;
    auto add = [&](Severity s, std::string where, std::string msg) { out.push_back({s, std::move(where), std::move(msg)}); };

    // ENTITY: a usable static model id is what controllers cache the model under.
    if (auto const* e = find_desc(descs, DESCRIPTOR_ENTITY, 0); (e != nullptr) && e->data.size() >= DescriptorEntity::LENGTH) {
        DescriptorEntity d{};
        span_load_padded(d, make_const_span(e->data));
        if (!d.entity_model_id.is_set()) {
            add(Severity::Error,
                "ENTITY",
                "entity_model_id is 0 — controllers cache the static AEM by this ID; an unset model ID breaks model "
                "caching/identification.");
        }
        if (d.configurations_count.get() == 0) {
            add(Severity::Error, "ENTITY", "configurations_count is 0.");
        }
    } else {
        add(Severity::Error, "ENTITY", "ENTITY descriptor missing or truncated.");
    }
}

/// AVB_INTERFACE: the descriptor a controller reads for network + gPTP identity.
void check_avb_interfaces(std::vector<RawDescriptor> const& descs, Add const& add)
{
    if (count_type(descs, DESCRIPTOR_AVB_INTERFACE) == 0) {
        add(Severity::Error, "AVB_INTERFACE", "no AVB_INTERFACE descriptor.");
    }
    for (auto const& r : descs) {
        if (r.type != DESCRIPTOR_AVB_INTERFACE) {
            continue;
        }
        std::string const where = std::format("AVB_INTERFACE[{}]", r.index);
        if (r.data.size() < DescriptorAvbInterface::MINIMUM_LENGTH) {
            add(Severity::Error, where, "truncated.");
            continue;
        }
        DescriptorAvbInterface a{};
        span_load_padded(a, make_const_span(r.data));
        if (!a.mac_address.is_set()) {
            add(Severity::Error, where, "mac_address is all-zero — it must carry the interface's real NIC MAC.");
        }
        if (!a.clock_identity.is_set()) {
            add(Severity::Error,
                where,
                "clock_identity is all-zero — must be the gPTP clock identity (typically MAC[0:3]:FF:FE:MAC[3:6]).");
        }
        if (a.clock_class.get() == 0 || a.clock_accuracy.get() == 0) {
            add(Severity::Warn,
                where,
                std::format(
                    "gPTP clock_class={} clock_accuracy={:#x} look unset; controllers show empty/invalid grandmaster info.",
                    a.clock_class.get(),
                    a.clock_accuracy.get()));
        }
    }
}

/// CLOCK_SOURCE: INTERNAL needs a location; an INPUT_STREAM source enables
/// "lock to incoming stream" selection.
void check_clock_sources(std::vector<RawDescriptor> const& descs, Add const& add)
{
    bool has_input_stream_clock = false;
    for (auto const& r : descs) {
        if (r.type != DESCRIPTOR_CLOCK_SOURCE) {
            continue;
        }
        std::string const where = std::format("CLOCK_SOURCE[{}]", r.index);
        if (r.data.size() < DescriptorClockSource::LENGTH) {
            add(Severity::Error, where, "truncated.");
            continue;
        }
        DescriptorClockSource c{};
        span_load_padded(c, make_const_span(r.data));
        if (c.clock_source_type.get() == CLOCK_SOURCE_TYPE_INPUT_STREAM) {
            has_input_stream_clock = true;
        }
        if (c.clock_source_type.get() == CLOCK_SOURCE_TYPE_INTERNAL && c.clock_source_location_type.get() == 0 &&
            c.clock_source_location_index.get() == 0) {
            add(Severity::Warn,
                where,
                "INTERNAL clock source has no clock_source_location reference (should point at the AUDIO_UNIT/entity).");
        }
    }
    if (!has_input_stream_clock && count_type(descs, DESCRIPTOR_CLOCK_SOURCE) > 0) {
        add(Severity::Info,
            "CLOCK_SOURCE",
            "no INPUT_STREAM clock source — a controller cannot select 'lock to incoming stream' on this entity.");
    }
}

/// JACKs: optional in the spec, but several controllers expect them.
void check_jacks(std::vector<RawDescriptor> const& descs, Add const& add)
{
    if (count_type(descs, DESCRIPTOR_JACK_INPUT) == 0 && count_type(descs, DESCRIPTOR_JACK_OUTPUT) == 0) {
        add(Severity::Info,
            "JACK",
            "no JACK_INPUT/JACK_OUTPUT descriptors — some controllers expect physical jacks to render the routing UI.");
    }
}

/// A descriptor with more formats than a 2021 one has room for (47 with the
/// formats at 132) is valid, and is told that a reader of the 2021 layout
/// sees one fewer.
void note_formats_left_out(Add const& add, std::string const& where, std::span<uint8_t const> const data)
{
    auto const sent = received_stream_formats(data);
    if (sent && sent->left_out() != 0) {
        add(Severity::Info,
            where,
            std::format(
                "carries {} formats, {} more than a descriptor with its formats at 138 (IEEE 1722.1-2021) has room for; "
                "all {} were read.",
                sent->count,
                sent->left_out(),
                sent->count));
    }
}

/// One STREAM descriptor: formats that lie inside it, a usable current_format
/// and a format list. The formats are read from where formats_offset says
/// (IEEE 1722.1-2021 7.2.6), and a sender that keeps them elsewhere than that
/// revision does is told so.
void check_stream(Add const& add, std::string const& where, std::span<uint8_t const> const data)
{
    auto const s = load_stream_descriptor(data);
    if (!s) {
        add(Severity::Error, where, "truncated, or its formats do not lie inside it.");
        return;
    }
    if (!s->current_format.is_set()) {
        add(Severity::Error, where, "current_format is 0.");
    }
    if (s->number_of_formats.get() == 0) {
        add(Severity::Warn, where, "number_of_formats is 0 — no advertised format list.");
    }
    if (auto const sent = received_stream_formats(data); sent && sent->count != 0 && sent->offset != DescriptorStream::LENGTH) {
        add(Severity::Info,
            where,
            std::format("formats_offset is {} where IEEE 1722.1-2021 has 138; the formats were read from there.", sent->offset));
    }
    note_formats_left_out(add, where, data);
}

/// STREAMs: a usable current_format and a format list.
void check_streams(std::vector<RawDescriptor> const& descs, Add const& add)
{
    for (auto const& r : descs) {
        if (r.type != DESCRIPTOR_STREAM_INPUT && r.type != DESCRIPTOR_STREAM_OUTPUT) {
            continue;
        }
        std::string const where =
            std::format("{}[{}]", r.type == DESCRIPTOR_STREAM_INPUT ? "STREAM_INPUT" : "STREAM_OUTPUT", r.index);
        check_stream(add, where, make_const_span(r.data));
    }
}

/// CONFIGURATION declared counts vs what was actually readable: a shortfall
/// means a controller's descriptor crawl stalls on the missing ones.
void check_configuration(std::vector<RawDescriptor> const& descs, Add const& add)
{
    if (auto const* cfg = find_desc(descs, DESCRIPTOR_CONFIGURATION, 0);
        (cfg != nullptr) && cfg->data.size() >= DescriptorConfiguration::LENGTH) {
        DescriptorConfiguration d{};
        span_load_padded(d, make_const_span(cfg->data));
        uint16_t const n = d.descriptor_counts_count.get();
        for (uint16_t i = 0; i < n && i < d.descriptor_counts.size(); ++i) {
            uint16_t const type = d.descriptor_counts[i].descriptor_type.get();
            uint16_t const declared = d.descriptor_counts[i].count.get();
            size_t const present = count_type(descs, type);
            if (present < declared) {
                add(Severity::Error,
                    std::format("CONFIGURATION type {:#06x}", type),
                    std::format("declares {} descriptors but only {} were readable.", declared, present));
            }
        }
    }
}

}  // namespace

auto validate_aem_model(std::vector<RawDescriptor> const& descs) -> std::vector<ModelFinding>
{
    std::vector<ModelFinding> out;
    Add const add = [&](Severity s, std::string where, std::string msg) { out.push_back({s, std::move(where), std::move(msg)}); };
    for (auto* const check :
         {&check_entity, &check_avb_interfaces, &check_clock_sources, &check_jacks, &check_streams, &check_configuration}) {
        check(descs, add);
    }
    std::stable_sort(out.begin(), out.end(), [](ModelFinding const& a, ModelFinding const& b) {
        return static_cast<int>(a.severity) > static_cast<int>(b.severity);
    });
    return out;
}

}  // namespace statusbar::atdecc_tools
