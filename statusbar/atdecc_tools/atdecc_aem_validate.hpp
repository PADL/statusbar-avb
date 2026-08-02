#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AEM entity-model validator — checks a crawled descriptor set against the
/// compliance rules a reference controller enforces, so model
/// problems are visible without a GUI. Pure (no I/O), unit-testable.

#include "statusbar/ieee/ieee.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::atdecc_tools {

enum class Severity : uint8_t
{
    Info,
    Warn,
    Error,
};

[[nodiscard]] inline auto severity_name(Severity s) -> std::string_view
{
    switch (s) {
        case Severity::Error:
            return "ERROR";
        case Severity::Warn:
            return "WARN";
        case Severity::Info:
            return "INFO";
    }
    return "?";
}

/// One validation finding against the model.
struct ModelFinding
{
    Severity severity{Severity::Info};
    std::string where;    ///< e.g. "AVB_INTERFACE[0]"
    std::string message;  ///< human-readable problem + why it matters
};

/// A raw descriptor as crawled off the wire (payload starting at descriptor_type).
struct RawDescriptor
{
    uint16_t type{0};
    uint16_t index{0};
    std::vector<uint8_t> data;
};

/// Validate a full crawled AEM model. `descs` should contain every descriptor a
/// READ_DESCRIPTOR crawl returned (ENTITY, CONFIGURATION, streams, AVB_INTERFACE,
/// CLOCK_*, jacks, …). Returns findings ordered Error → Warn → Info.
[[nodiscard]] auto validate_aem_model(std::vector<RawDescriptor> const& descs) -> std::vector<ModelFinding>;

}  // namespace statusbar::atdecc_tools
