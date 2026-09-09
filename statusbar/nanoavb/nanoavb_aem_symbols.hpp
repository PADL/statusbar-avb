#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_aem_fingerprint.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Controller-derived symbol tables. Project files bind
// to symbols, never to descriptor_index and never to a raw handle. The
// rules, verbatim from the plan:
//
//   1. Never derive from a flat descriptor_index.
//   2. Walk the ownership tree top-down using base_x / number_of_x,
//      emitting (owner role, local ordinal) segments.
//   3. Discriminate CONTROLs by control_type, ordinal within siblings of
//      the same type, so adding a delay control does not renumber gains.
//   4. Never key on object_name (mutable via SET_NAME).
//   5. MATRIX/MIXER crosspoints extend the path with h/w (at binding
//      time; the table itself carries the descriptor symbol).
//   6. Unreachable descriptors get orphan/<type>/<index> and are flagged,
//      not dropped.
//
// The generator is versioned; store the version with every generated
// table and project file so an old project resolves through the table it
// was authored against.
//
// Version history:
//   1  AUDIO_UNIT trees only (blocks, controls, selectors, matrices).
//   2  Every owner in the standard: AUDIO/VIDEO/SENSOR_UNIT trees gain
//      ports (spin/spout/extin/extout/intin/intout), the ports' controls,
//      clusters and maps, and mixers; JACK, AVB_INTERFACE (2021) and
//      PTP_INSTANCE controls scope under their owner; the controls the
//      CONFIGURATION declares in descriptor_counts become
//      cfgN/ctl:<type>/<ordinal>. Port/cluster/map descriptors no unit
//      reaches are orphans (v1 enumerated them per type).

namespace statusbar::nanoavb {

inline constexpr std::uint32_t AEM_SYMBOL_GENERATOR_VERSION = 2;

struct AemSymbol
{
    std::uint16_t configuration = 0;
    std::uint16_t descriptor_type = 0;
    std::uint16_t descriptor_index = 0;
    std::string symbol;
    bool orphan = false;
};

/// Derives the symbol table from a crawled or generated descriptor set.
/// Every descriptor gets exactly one entry.
std::vector<AemSymbol> derive_aem_symbols(std::span<AemRawDescriptor const> descriptors);

}  // namespace statusbar::nanoavb
