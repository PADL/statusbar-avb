// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// walk_capture(): the same packets come out of a classic pcap and a pcapng file, and a
/// file that is neither, or is not there, is an error.

#include "statusbar/avtp/avtp_capture_walk.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/pcap/pcap.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

using namespace statusbar;
using statusbar::ieee::Eui48;

namespace {

/// What walk_capture() said of one packet.
struct Seen
{
    uint64_t timestamp_us{0};
    Eui48 da{};
    Eui48 sa{};
    uint16_t ethertype{0};
    std::vector<uint8_t> payload{};

    auto operator==(Seen const&) const -> bool = default;
};

/// A file of this process alone under /tmp: the same tests also run inside the
/// all-tests binary, at the same time, and must not share their files with it.
auto scratch_file(std::string_view name) -> std::string
{
    return std::format("/tmp/sb_capture_walk_{}_{}", ::getpid(), name);
}

/// An Ethernet frame of ethertype 0x22F0 from @p who with @p octets of payload.
auto frame(uint8_t who, size_t octets) -> std::vector<uint8_t>
{
    std::vector<uint8_t> f{0x91, 0xE0, 0xF0, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, who, 0x22, 0xF0};
    for (size_t i = 0; i < octets; ++i) {
        f.push_back(static_cast<uint8_t>(who + i));
    }
    return f;
}

/// Three frames written to @p path through @p Writer.
template <typename Writer>
auto write_capture(std::string const& path) -> bool
{
    auto writer = Writer::open(path);
    if (!writer) {
        return false;
    }
    bool ok = true;
    for (uint8_t who = 1; who <= 3; ++who) {
        ok = ok && writer->write_packet(1'000'000ULL * who, frame(who, 46 + who)).has_value();
    }
    return ok;
}

/// Every packet walk_capture() reports for @p path; empty when it fails.
auto walked(std::string const& path) -> std::vector<Seen>
{
    std::vector<Seen> seen;
    auto const walk = avtp::walk_capture(
        path, [&](uint64_t timestamp_us, Eui48 const& da, Eui48 const& sa, uint16_t ethertype, pcap::Packet const& payload) {
            seen.push_back({timestamp_us, da, sa, ethertype, payload});
        });
    return walk ? seen : std::vector<Seen>{};
}

}  // namespace

TEST(avtp_capture_walk, a_classic_pcap_and_a_pcapng_give_the_same_packets)
{
    std::string const classic = scratch_file("classic.pcap");
    std::string const ng = scratch_file("ng.pcapng");
    EXPECT_TRUE(write_capture<pcap::FileWriter>(classic));
    EXPECT_TRUE(write_capture<pcap::PcapngFileWriter>(ng));

    auto const from_classic = walked(classic);
    auto const from_ng = walked(ng);
    EXPECT_EQ(from_classic.size(), size_t{3});
    EXPECT_TRUE(from_classic == from_ng);
    EXPECT_EQ(from_classic[1].ethertype, 0x22F0);
    EXPECT_EQ(from_classic[1].payload.size(), size_t{48});
    EXPECT_TRUE(from_classic[2].sa == (Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x03}));
    EXPECT_EQ(from_classic[2].timestamp_us - from_classic[0].timestamp_us, uint64_t{2'000'000});
    (void)std::remove(classic.c_str());
    (void)std::remove(ng.c_str());
}

TEST(avtp_capture_walk, a_file_that_is_neither_is_refused_by_its_magic)
{
    std::string const path = scratch_file("text.txt");
    std::FILE* const f = std::fopen(path.c_str(), "wb");
    EXPECT_TRUE(f != nullptr);
    (void)std::fputs("not a capture at all\n", f);
    (void)std::fclose(f);

    size_t packets = 0;
    auto const walk =
        avtp::walk_capture(path, [&](uint64_t, Eui48 const&, Eui48 const&, uint16_t, pcap::Packet const&) { ++packets; });
    EXPECT_FALSE(walk.has_value());
    EXPECT_TRUE(walk.error() == pcap::make_error_code(pcap::PcapError::incompatible_magic));
    EXPECT_EQ(packets, size_t{0});
    (void)std::remove(path.c_str());
}

TEST(avtp_capture_walk, a_file_that_is_not_there_is_an_open_error)
{
    auto const walk = avtp::walk_capture(
        scratch_file("no_such_file.pcap"), [](uint64_t, Eui48 const&, Eui48 const&, uint16_t, pcap::Packet const&) {});
    EXPECT_FALSE(walk.has_value());
    EXPECT_TRUE(walk.error() == pcap::make_error_code(pcap::PcapError::file_open_failed));
}

TEST_MAIN(statusbar_avtp, avtp_capture_walk_test)
