// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_aem_symbols.hpp"

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <cstdio>
#include <map>
#include <tuple>

namespace statusbar::nanoavb {

using namespace statusbar::atdecc::aem;

namespace {

using Key = std::tuple<std::uint16_t, std::uint16_t, std::uint16_t>;  // config, type, index

/// Short segment names for configuration-level descriptor types; anything
/// absent falls back to t<hex>.
std::string type_segment(std::uint16_t type)
{
    switch (type) {
        case DESCRIPTOR_AUDIO_UNIT:
            return "au";
        case DESCRIPTOR_VIDEO_UNIT:
            return "vu";
        case DESCRIPTOR_SENSOR_UNIT:
            return "su";
        case DESCRIPTOR_STREAM_INPUT:
            return "strin";
        case DESCRIPTOR_STREAM_OUTPUT:
            return "strout";
        case DESCRIPTOR_JACK_INPUT:
            return "jackin";
        case DESCRIPTOR_JACK_OUTPUT:
            return "jackout";
        case DESCRIPTOR_AVB_INTERFACE:
            return "avbif";
        case DESCRIPTOR_CLOCK_SOURCE:
            return "clksrc";
        case DESCRIPTOR_CLOCK_DOMAIN:
            return "clkdom";
        case DESCRIPTOR_MEMORY_OBJECT:
            return "memobj";
        case DESCRIPTOR_LOCALE:
            return "locale";
        case DESCRIPTOR_STRINGS:
            return "strings";
        case DESCRIPTOR_TIMING:
            return "timing";
        case DESCRIPTOR_PTP_INSTANCE:
            return "ptpinst";
        case DESCRIPTOR_PTP_PORT:
            return "ptpport";
        default: {
            char buf[8];
            snprintf(buf, sizeof buf, "t%04x", type);
            return buf;
        }
    }
}

/// A control_type as a symbol segment: the registry name lowercased when
/// it knows the type (Table 7.4 or a known vendor type such as
/// MEYER_FAN_STATUS / JDKS_IPV4_PARAMETERS), else the EUI-64 as 16 hex
/// digits — never a non-identifying placeholder.
std::string control_type_segment(ieee::Eui64 const& type)
{
    auto name = control_type_name(type);
    bool identifying = !name.empty();
    for (char c : name) {
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') {
            identifying = false;
            break;
        }
    }
    std::string lowered;
    if (identifying) {
        for (char c : name) {
            lowered.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
        }
        if (lowered == "reserved" || lowered == "vendor" || lowered == "vendor_defined" || lowered == "unknown" ||
            lowered == "expansion") {
            identifying = false;
        }
    }
    if (identifying) {
        return lowered;
    }
    char buf[20];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)type.get());
    return buf;
}

/// The descriptor types only ever reached through an owner's base_x /
/// number_of_x range (rule 6): unreached instances are orphans, whereas
/// every other type is configuration-owned and enumerates per type.
bool owner_reached_only(std::uint16_t type)
{
    switch (type) {
        case DESCRIPTOR_CONTROL:
        case DESCRIPTOR_CONTROL_BLOCK:
        case DESCRIPTOR_SIGNAL_SELECTOR:
        case DESCRIPTOR_MIXER:
        case DESCRIPTOR_MATRIX:
        case DESCRIPTOR_MATRIX_SIGNAL:
        case DESCRIPTOR_STREAM_PORT_INPUT:
        case DESCRIPTOR_STREAM_PORT_OUTPUT:
        case DESCRIPTOR_EXTERNAL_PORT_INPUT:
        case DESCRIPTOR_EXTERNAL_PORT_OUTPUT:
        case DESCRIPTOR_INTERNAL_PORT_INPUT:
        case DESCRIPTOR_INTERNAL_PORT_OUTPUT:
        case DESCRIPTOR_AUDIO_CLUSTER:
        case DESCRIPTOR_VIDEO_CLUSTER:
        case DESCRIPTOR_SENSOR_CLUSTER:
        case DESCRIPTOR_AUDIO_MAP:
        case DESCRIPTOR_VIDEO_MAP:
        case DESCRIPTOR_SENSOR_MAP:
            return true;
        default:
            return false;
    }
}

struct Walker
{
    std::span<AemRawDescriptor const> descriptors;
    std::map<Key, AemRawDescriptor const*> by_key;
    std::map<Key, std::string> symbols;

    AemRawDescriptor const* find(std::uint16_t config, std::uint16_t type, std::uint16_t index) const
    {
        auto it = by_key.find({config, type, index});
        return it == by_key.end() ? nullptr : it->second;
    }

    bool claim(std::uint16_t config, std::uint16_t type, std::uint16_t index, std::string symbol)
    {
        return symbols.emplace(Key{config, type, index}, std::move(symbol)).second;
    }

    /// Controls in [base, base+count) get per-control_type ordinals under
    /// @p owner (§6.3 rule 3).
    void claim_controls(std::uint16_t config, std::string const& owner, std::uint16_t base, std::uint16_t count)
    {
        std::map<std::string, unsigned> ordinals;
        for (std::uint16_t i = 0; i < count; ++i) {
            auto index = std::uint16_t(base + i);
            auto const* raw = find(config, DESCRIPTOR_CONTROL, index);
            if (raw == nullptr || symbols.contains(Key{config, DESCRIPTOR_CONTROL, index})) {
                continue;
            }
            DescriptorControl c;
            span_load_padded(c, raw->bytes);
            auto segment = control_type_segment(c.control_type);
            auto ordinal = ordinals[segment]++;
            (void)claim(config, DESCRIPTOR_CONTROL, index, owner + "/ctl:" + segment + "/" + std::to_string(ordinal));
        }
    }

    /// Descriptors of @p type in [base, base+count) get "<owner>/<segment><ordinal>".
    void claim_range(
        std::uint16_t config,
        std::string const& owner,
        std::uint16_t type,
        char const* segment,
        std::uint16_t base,
        std::uint16_t count)
    {
        for (std::uint16_t i = 0; i < count; ++i) {
            (void)claim(config, type, std::uint16_t(base + i), owner + "/" + segment + std::to_string(i));
        }
    }

    /// A unit's ports of one kind: the port itself, its controls, and (for
    /// stream ports) its clusters and maps. All six port descriptors keep
    /// number_of_controls / base_control at the same offset; the two
    /// STREAM_PORT layouts additionally carry cluster and map ranges.
    void walk_ports(
        std::uint16_t config,
        std::string const& unit_symbol,
        std::uint16_t port_type,
        char const* segment,
        std::uint16_t base,
        std::uint16_t count,
        std::uint16_t cluster_type,
        std::uint16_t map_type)
    {
        for (std::uint16_t p = 0; p < count; ++p) {
            auto port_index = std::uint16_t(base + p);
            auto const* raw = find(config, port_type, port_index);
            if (raw == nullptr) {
                continue;
            }
            auto port_symbol = unit_symbol + "/" + segment + std::to_string(p);
            (void)claim(config, port_type, port_index, port_symbol);
            if (port_type == DESCRIPTOR_STREAM_PORT_INPUT || port_type == DESCRIPTOR_STREAM_PORT_OUTPUT) {
                DescriptorStreamPort port;
                span_load_padded(port, raw->bytes);
                claim_controls(config, port_symbol, port.base_control.get(), port.number_of_controls.get());
                claim_range(config, port_symbol, cluster_type, "clus", port.base_cluster.get(), port.number_of_clusters.get());
                claim_range(config, port_symbol, map_type, "map", port.base_map.get(), port.number_of_maps.get());
            } else {
                DescriptorExternalPort port;  // INTERNAL_PORT shares the control range layout
                span_load_padded(port, raw->bytes);
                claim_controls(config, port_symbol, port.base_control.get(), port.number_of_controls.get());
            }
        }
    }

    /// AUDIO_UNIT, VIDEO_UNIT and SENSOR_UNIT share one ownership layout
    /// (Clause 7.2.3–7.2.5): the walk is the same, only the cluster / map
    /// types under the stream ports differ.
    template <class Unit>
    void walk_unit(
        std::uint16_t config,
        std::string const& cfg_symbol,
        std::uint16_t unit_type,
        std::uint16_t unit_index,
        unsigned unit_ordinal,
        std::uint16_t cluster_type,
        std::uint16_t map_type)
    {
        auto const* raw = find(config, unit_type, unit_index);
        if (raw == nullptr) {
            return;
        }
        auto unit_symbol = cfg_symbol + "/" + type_segment(unit_type) + std::to_string(unit_ordinal);
        (void)claim(config, unit_type, unit_index, unit_symbol);

        Unit unit;
        span_load_padded(unit, raw->bytes);

        // CONTROL_BLOCKs claim their control ranges first, so a control's
        // symbol is scoped to its strip and adding a strip does not
        // renumber another strip's controls.
        for (std::uint16_t b = 0; b < unit.number_of_control_blocks.get(); ++b) {
            auto block_index = std::uint16_t(unit.base_control_block.get() + b);
            auto const* block_raw = find(config, DESCRIPTOR_CONTROL_BLOCK, block_index);
            if (block_raw == nullptr) {
                continue;
            }
            auto block_symbol = unit_symbol + "/cb" + std::to_string(b);
            (void)claim(config, DESCRIPTOR_CONTROL_BLOCK, block_index, block_symbol);
            DescriptorControlBlock block;
            span_load_padded(block, block_raw->bytes);
            claim_controls(config, block_symbol, block.base_control.get(), block.number_of_controls.get());
        }

        // Unit-owned controls outside every block.
        claim_controls(config, unit_symbol, unit.base_control.get(), unit.number_of_controls.get());

        walk_ports(
            config,
            unit_symbol,
            DESCRIPTOR_STREAM_PORT_INPUT,
            "spin",
            unit.base_stream_input_port.get(),
            unit.number_of_stream_input_ports.get(),
            cluster_type,
            map_type);
        walk_ports(
            config,
            unit_symbol,
            DESCRIPTOR_STREAM_PORT_OUTPUT,
            "spout",
            unit.base_stream_output_port.get(),
            unit.number_of_stream_output_ports.get(),
            cluster_type,
            map_type);
        walk_ports(
            config,
            unit_symbol,
            DESCRIPTOR_EXTERNAL_PORT_INPUT,
            "extin",
            unit.base_external_input_port.get(),
            unit.number_of_external_input_ports.get(),
            cluster_type,
            map_type);
        walk_ports(
            config,
            unit_symbol,
            DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
            "extout",
            unit.base_external_output_port.get(),
            unit.number_of_external_output_ports.get(),
            cluster_type,
            map_type);
        walk_ports(
            config,
            unit_symbol,
            DESCRIPTOR_INTERNAL_PORT_INPUT,
            "intin",
            unit.base_internal_input_port.get(),
            unit.number_of_internal_input_ports.get(),
            cluster_type,
            map_type);
        walk_ports(
            config,
            unit_symbol,
            DESCRIPTOR_INTERNAL_PORT_OUTPUT,
            "intout",
            unit.base_internal_output_port.get(),
            unit.number_of_internal_output_ports.get(),
            cluster_type,
            map_type);

        claim_range(
            config,
            unit_symbol,
            DESCRIPTOR_SIGNAL_SELECTOR,
            "sel",
            unit.base_signal_selector.get(),
            unit.number_of_signal_selectors.get());
        claim_range(config, unit_symbol, DESCRIPTOR_MIXER, "mix", unit.base_mixer.get(), unit.number_of_mixers.get());

        std::map<std::string, unsigned> matrix_ordinals;
        unsigned signal_ordinal = 0;
        for (std::uint16_t m = 0; m < unit.number_of_matrices.get(); ++m) {
            auto matrix_index = std::uint16_t(unit.base_matrix.get() + m);
            auto const* matrix_raw = find(config, DESCRIPTOR_MATRIX, matrix_index);
            if (matrix_raw == nullptr) {
                continue;
            }
            DescriptorMatrix matrix;
            span_load_padded(matrix, matrix_raw->bytes);
            auto segment = control_type_segment(matrix.control_type);
            auto ordinal = matrix_ordinals[segment]++;
            (void)claim(config, DESCRIPTOR_MATRIX, matrix_index, unit_symbol + "/mx:" + segment + "/" + std::to_string(ordinal));

            // MATRIX_SIGNALs are claimed through the matrices referencing
            // them; shared source lists claim once, in walk order.
            for (std::uint16_t s = 0; s < matrix.number_of_sources.get(); ++s) {
                auto signal_index = std::uint16_t(matrix.base_source.get() + s);
                if (claim(
                        config, DESCRIPTOR_MATRIX_SIGNAL, signal_index, unit_symbol + "/mxsig" + std::to_string(signal_ordinal))) {
                    ++signal_ordinal;
                }
            }
        }
    }

    /// Configuration-level owners of controls: JACKs, 2021 AVB_INTERFACEs
    /// (a 2013 payload stops before the control range and claims nothing)
    /// and PTP_INSTANCEs. Their own symbols are the per-type enumeration.
    void walk_config_owner(std::uint16_t config, std::string const& cfg_symbol, std::uint16_t type, std::uint16_t index)
    {
        auto const* raw = find(config, type, index);
        if (raw == nullptr) {
            return;
        }
        auto owner_symbol = cfg_symbol + "/" + type_segment(type) + std::to_string(index);
        switch (type) {
            case DESCRIPTOR_JACK_INPUT:
            case DESCRIPTOR_JACK_OUTPUT: {
                DescriptorJack jack;
                span_load_padded(jack, raw->bytes);
                claim_controls(config, owner_symbol, jack.base_control.get(), jack.number_of_controls.get());
                break;
            }
            case DESCRIPTOR_AVB_INTERFACE: {
                if (raw->bytes.size() < DescriptorAvbInterface::LENGTH) {
                    break;
                }
                DescriptorAvbInterface iface;
                span_load_padded(iface, raw->bytes);
                claim_controls(config, owner_symbol, iface.base_control.get(), iface.number_of_controls.get());
                break;
            }
            case DESCRIPTOR_PTP_INSTANCE: {
                DescriptorPtpInstance ptp;
                span_load_padded(ptp, raw->bytes);
                claim_controls(config, owner_symbol, ptp.base_control.get(), ptp.number_of_controls.get());
                break;
            }
            default:
                break;
        }
    }

    /// Top-level controls: the CONFIGURATION's descriptor_counts declares
    /// how many CONTROLs it owns directly (Clause 7.2.2) but not which
    /// indexes, so the first `count` controls no owner reached — in index
    /// order — are the configuration's; anything beyond stays an orphan.
    void claim_configuration_controls(std::uint16_t config, std::string const& cfg_symbol)
    {
        AemRawDescriptor const* raw = find(config, DESCRIPTOR_CONFIGURATION, config);
        if (raw == nullptr) {
            for (auto const& [key, candidate] : by_key) {
                if (std::get<0>(key) == config && std::get<1>(key) == DESCRIPTOR_CONFIGURATION) {
                    raw = candidate;
                    break;
                }
            }
        }
        if (raw == nullptr) {
            return;
        }
        DescriptorConfiguration cfg;
        span_load_padded(cfg, raw->bytes);
        std::uint16_t declared = 0;
        for (auto const& entry : cfg.used_descriptor_counts()) {
            if (entry.descriptor_type.get() == DESCRIPTOR_CONTROL) {
                declared = entry.count.get();
                break;
            }
        }

        std::map<std::string, unsigned> ordinals;
        for (auto const& [key, control_raw] : by_key) {
            if (declared == 0) {
                break;
            }
            auto [key_config, type, index] = key;
            if (key_config != config || type != DESCRIPTOR_CONTROL || symbols.contains(key)) {
                continue;
            }
            DescriptorControl c;
            span_load_padded(c, control_raw->bytes);
            auto segment = control_type_segment(c.control_type);
            auto ordinal = ordinals[segment]++;
            (void)claim(config, DESCRIPTOR_CONTROL, index, cfg_symbol + "/ctl:" + segment + "/" + std::to_string(ordinal));
            --declared;
        }
    }
};

}  // namespace

std::vector<AemSymbol> derive_aem_symbols(std::span<AemRawDescriptor const> descriptors)
{
    Walker w{descriptors, {}, {}};
    for (auto const& d : descriptors) {
        w.by_key[{d.configuration, d.descriptor_type, d.descriptor_index}] = &d;
    }

    // ENTITY, then per configuration the owned trees (units first, then
    // the configuration-level owners, then the configuration's own
    // controls), then the remaining configuration-level types by
    // (type, index-within-type) ordinal.
    for (auto const& [key, raw] : w.by_key) {
        auto [config, type, index] = key;
        if (type == DESCRIPTOR_ENTITY) {
            (void)w.claim(config, type, index, "entity");
        } else if (type == DESCRIPTOR_CONFIGURATION) {
            (void)w.claim(config, type, index, "cfg" + std::to_string(index));
        }
    }
    for (auto const& [key, raw] : w.by_key) {
        auto [config, type, index] = key;
        auto cfg_symbol = "cfg" + std::to_string(config);
        // ordinal == index within the configuration's units of that type.
        if (type == DESCRIPTOR_AUDIO_UNIT) {
            w.walk_unit<DescriptorAudioUnit>(
                config, cfg_symbol, type, index, index, DESCRIPTOR_AUDIO_CLUSTER, DESCRIPTOR_AUDIO_MAP);
        } else if (type == DESCRIPTOR_VIDEO_UNIT) {
            w.walk_unit<DescriptorVideoUnit>(
                config, cfg_symbol, type, index, index, DESCRIPTOR_VIDEO_CLUSTER, DESCRIPTOR_VIDEO_MAP);
        } else if (type == DESCRIPTOR_SENSOR_UNIT) {
            w.walk_unit<DescriptorSensorUnit>(
                config, cfg_symbol, type, index, index, DESCRIPTOR_SENSOR_CLUSTER, DESCRIPTOR_SENSOR_MAP);
        }
    }
    for (auto const& [key, raw] : w.by_key) {
        auto [config, type, index] = key;
        if (type == DESCRIPTOR_JACK_INPUT || type == DESCRIPTOR_JACK_OUTPUT || type == DESCRIPTOR_AVB_INTERFACE ||
            type == DESCRIPTOR_PTP_INSTANCE) {
            w.walk_config_owner(config, "cfg" + std::to_string(config), type, index);
        }
    }
    for (auto const& [key, raw] : w.by_key) {
        auto [config, type, index] = key;
        if (type == DESCRIPTOR_CONFIGURATION) {
            w.claim_configuration_controls(config, "cfg" + std::to_string(config));
        }
    }

    std::vector<AemSymbol> out;
    out.reserve(descriptors.size());
    for (auto const& [key, raw] : w.by_key) {
        auto [config, type, index] = key;
        auto it = w.symbols.find(key);
        AemSymbol entry{config, type, index, {}, false};
        if (it != w.symbols.end()) {
            entry.symbol = it->second;
        } else if (!owner_reached_only(type)) {
            entry.symbol = "cfg" + std::to_string(config) + "/" + type_segment(type) + std::to_string(index);
        } else {
            char buf[24];
            snprintf(buf, sizeof buf, "orphan/%04x/%u", type, index);
            entry.symbol = buf;
            entry.orphan = true;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

}  // namespace statusbar::nanoavb
