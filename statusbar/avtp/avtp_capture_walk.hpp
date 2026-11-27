#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Walking a capture file whatever its container. A tool that reads a capture is handed
/// what tcpdump wrote (classic pcap) as often as what a pcapng writer did, and the
/// two have different readers: this picks the one the file's first four octets ask
/// for, so a tool takes either.

#include "statusbar/avtp/avtp_capture_frame.hpp"
#include "statusbar/pcap/pcap_error.hpp"
#include "statusbar/pcap/pcap_reader.hpp"
#include "statusbar/pcap/pcapng_reader.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

namespace statusbar::avtp {

/// The capture's first four octets, or nullopt when the file cannot be read that far.
[[nodiscard]] inline auto capture_magic(std::string const& path) -> std::optional<std::array<uint8_t, 4>>
{
    std::unique_ptr<FILE, int (*)(FILE*)> const file{std::fopen(path.c_str(), "rb"), &std::fclose};
    std::array<uint8_t, 4> magic{};
    if (!file || std::fread(magic.data(), 1, magic.size(), file.get()) != magic.size()) {
        return std::nullopt;
    }
    return magic;
}

namespace detail {

/// Open the capture with @p Reader and walk it.
template <typename Reader, typename OnPacket>
[[nodiscard]] auto walk_capture_with(std::string const& path, OnPacket const& on_packet) -> Status
{
    auto reader = Reader::open(path);
    if (!reader) {
        return failure(reader.error());
    }
    return pcap::for_each_packet(*reader, on_packet);
}

}  // namespace detail

/// Walk a capture whatever its container: classic pcap through pcap::FileReader,
/// pcapng through pcap::PcapngReader, as the first four octets say.
///
/// @param on_packet Called for each packet as pcap::for_each_packet() calls it:
///        (timestamp_us, da, sa, ethertype, payload).
/// @return The reader's error when the file does not open or does not read through;
///         PcapError::file_open_failed when its first four octets cannot be read, and
///         PcapError::incompatible_magic when they are those of neither container.
template <typename OnPacket>
[[nodiscard]] auto walk_capture(std::string const& path, OnPacket const& on_packet) -> Status
{
    auto const magic = capture_magic(path);
    if (!magic) {
        return failure(pcap::PcapError::file_open_failed);
    }
    switch (capture_format_of(*magic)) {
        case CaptureFormat::pcapng:
            return detail::walk_capture_with<pcap::PcapngReader>(path, on_packet);
        case CaptureFormat::pcap:
            return detail::walk_capture_with<pcap::FileReader>(path, on_packet);
        case CaptureFormat::unknown:
            break;
    }
    return failure(pcap::PcapError::incompatible_magic);
}

}  // namespace statusbar::avtp
