#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The link an entity's AVB streams use (AF_PACKET or AF_XDP) and how an
/// AF_XDP socket binds: the configuration words and their spellings.

#include <cstdint>
#include <optional>
#include <string_view>

namespace statusbar::avb_entity {

/// Which StreamPort carries the streams. packet: AF_PACKET (macOS: BPF);
/// xdp: AF_XDP or nothing; auto_: AF_XDP when it opens, else AF_PACKET.
enum class StreamTransport : uint8_t
{
    packet,
    xdp,
    auto_,
};

/// How the AF_XDP socket binds. auto_: native attach, zero-copy when the
/// driver offers it; zerocopy: native and zero-copy or fail; copy: generic
/// attach and copy mode (no adapter reset).
enum class StreamXdpMode : uint8_t
{
    auto_,
    zerocopy,
    copy,
};

/// A StreamTransport's spelling: "packet", "xdp" or "auto".
[[nodiscard]] constexpr auto stream_transport_name(StreamTransport const transport) noexcept -> std::string_view
{
    switch (transport) {
        case StreamTransport::xdp:
            return "xdp";
        case StreamTransport::auto_:
            return "auto";
        case StreamTransport::packet:
            break;
    }
    return "packet";
}

/// The StreamTransport spelled @p word, or nullopt.
[[nodiscard]] constexpr auto parse_stream_transport(std::string_view const word) noexcept -> std::optional<StreamTransport>
{
    for (auto const transport : {StreamTransport::packet, StreamTransport::xdp, StreamTransport::auto_}) {
        if (word == stream_transport_name(transport)) {
            return transport;
        }
    }
    return std::nullopt;
}

/// A StreamXdpMode's spelling: "auto", "zerocopy" or "copy".
[[nodiscard]] constexpr auto stream_xdp_mode_name(StreamXdpMode const mode) noexcept -> std::string_view
{
    switch (mode) {
        case StreamXdpMode::zerocopy:
            return "zerocopy";
        case StreamXdpMode::copy:
            return "copy";
        case StreamXdpMode::auto_:
            break;
    }
    return "auto";
}

/// The StreamXdpMode spelled @p word, or nullopt.
[[nodiscard]] constexpr auto parse_stream_xdp_mode(std::string_view const word) noexcept -> std::optional<StreamXdpMode>
{
    for (auto const mode : {StreamXdpMode::auto_, StreamXdpMode::zerocopy, StreamXdpMode::copy}) {
        if (word == stream_xdp_mode_name(mode)) {
            return mode;
        }
    }
    return std::nullopt;
}

}  // namespace statusbar::avb_entity
