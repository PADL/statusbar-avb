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
        default: {
            char buf[8];
            snprintf(buf, sizeof buf, "t%04x", type);
            return buf;
        }
    }
}

/// A control_type as a symbol segment: the standard name lowercased when
/// the registry knows it, else the EUI-64 as 16 hex digits — never a
/// non-identifying placeholder.
std::string control_type_segment(ieee::Eui64 const& type)
{
    auto name = control_type_name(type);
    bool identifying = !name.empty();
    for (char c : name) {
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && c != '_') {
            identifying = false;
            break;
        }
    }
    std::string lowered;
    if (identifying) {
        for (char c : name) {
            lowered.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
        }
        if (lowered == "reserved" || lowered == "vendor" || lowered == "unknown" || lowered == "expansion") {
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

    void walk_audio_unit(std::uint16_t config, std::string const& cfg_symbol, std::uint16_t unit_index, unsigned unit_ordinal)
    {
        auto const* raw = find(config, DESCRIPTOR_AUDIO_UNIT, unit_index);
        if (raw == nullptr) {
            return;
        }
        auto unit_symbol = cfg_symbol + "/au" + std::to_string(unit_ordinal);
        (void)claim(config, DESCRIPTOR_AUDIO_UNIT, unit_index, unit_symbol);

        DescriptorAudioUnit unit;
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

        for (std::uint16_t s = 0; s < unit.number_of_signal_selectors.get(); ++s) {
            (void)claim(
                config,
                DESCRIPTOR_SIGNAL_SELECTOR,
                std::uint16_t(unit.base_signal_selector.get() + s),
                unit_symbol + "/sel" + std::to_string(s));
        }

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
};

}  // namespace

std::vector<AemSymbol> derive_aem_symbols(std::span<AemRawDescriptor const> descriptors)
{
    Walker w{descriptors, {}, {}};
    for (auto const& d : descriptors) {
        w.by_key[{d.configuration, d.descriptor_type, d.descriptor_index}] = &d;
    }

    // ENTITY, then per configuration the owned tree, then the remaining
    // configuration-level types by (type, index-within-type) ordinal.
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
        if (type == DESCRIPTOR_AUDIO_UNIT) {
            // ordinal == index within the configuration's audio units.
            w.walk_audio_unit(config, "cfg" + std::to_string(config), index, index);
        }
    }

    // The unit-owned types stay orphaned when no unit reached them; every
    // other type is configuration-owned and enumerates per type.
    auto unit_owned = [](std::uint16_t type) {
        return type == DESCRIPTOR_CONTROL || type == DESCRIPTOR_CONTROL_BLOCK || type == DESCRIPTOR_SIGNAL_SELECTOR ||
            type == DESCRIPTOR_MATRIX || type == DESCRIPTOR_MATRIX_SIGNAL || type == DESCRIPTOR_MIXER;
    };

    std::vector<AemSymbol> out;
    out.reserve(descriptors.size());
    for (auto const& [key, raw] : w.by_key) {
        auto [config, type, index] = key;
        auto it = w.symbols.find(key);
        AemSymbol entry{config, type, index, {}, false};
        if (it != w.symbols.end()) {
            entry.symbol = it->second;
        } else if (!unit_owned(type)) {
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
