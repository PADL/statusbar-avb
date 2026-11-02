// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Unit tests for TxPcapRecorder -- the RT-safe in-memory capture of the entity's
/// own (qdisc-bypassed, otherwise unobservable) transmitted stream frames, and the
/// non-RT flush to a libpcap file.

#include "statusbar/avb_entity/tx_pcap_recorder.hpp"

#include "statusbar/pcap/pcap_reader.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using statusbar::is_success;
using statusbar::avb_entity::TxPcapRecorder;

namespace {

auto make_frame(uint8_t tag, size_t len) -> std::vector<uint8_t>
{
    std::vector<uint8_t> f(len);
    for (size_t i = 0; i < len; ++i) {
        f[i] = static_cast<uint8_t>(tag + i);
    }
    return f;
}

// Read every packet back from a written pcap; return the recovered frames.
auto read_back(std::string const& path) -> std::vector<std::vector<uint8_t>>
{
    std::vector<std::vector<uint8_t>> out;
    auto reader = statusbar::pcap::FileReader::open(path);
    if (!reader) {
        return out;
    }
    statusbar::pcap::Packet pkt;
    uint64_t ts = 0;
    for (;;) {
        auto const r = reader->read_packet(&ts, pkt);
        if (!r || !r.value()) {
            break;  // error or clean EOF
        }
        out.emplace_back(pkt.begin(), pkt.end());
    }
    return out;
}

// The wire length each record reports, which is what tells a reader a
// capture was snapped.
auto read_back_wire_lengths(std::string const& path) -> std::vector<uint32_t>
{
    std::vector<uint32_t> out;
    auto reader = statusbar::pcap::FileReader::open(path);
    if (!reader) {
        return out;
    }
    statusbar::pcap::Packet pkt;
    uint64_t ts = 0;
    uint32_t original = 0;
    for (;;) {
        auto const r = reader->read_packet(&ts, pkt, &original);
        if (!r || !r.value()) {
            break;
        }
        out.push_back(original);
    }
    return out;
}

}  // namespace

TEST(tx_pcap_recorder, window_closes_after_duration_and_writes_file)
{
    TxPcapRecorder rec;
    std::string const path = "/tmp/sb_txpcap_window.pcap";
    rec.configure(path, size_t{1} << 20, /*snaplen=*/1522, /*duration_ns=*/1'000'000ULL);  // 1 ms window
    EXPECT_TRUE(rec.recording());

    auto const f = make_frame(0x10, 64);
    rec.record(f, 1'000'000ULL);  // first frame arms the window at t0
    rec.record(f, 1'100'000ULL);  // +100 us -- inside
    rec.record(f, 1'500'000ULL);  // +500 us -- inside
    EXPECT_TRUE(rec.recording());
    EXPECT_EQ(rec.frame_count(), 3U);

    rec.record(f, 2'000'001ULL);  // > 1 ms past t0 -- closes the window, NOT recorded
    EXPECT_FALSE(rec.recording());
    EXPECT_TRUE(rec.ready_to_write());
    EXPECT_EQ(rec.frame_count(), 3U);

    rec.record(f, 2'500'000ULL);  // after close -- ignored
    EXPECT_EQ(rec.frame_count(), 3U);

    EXPECT_TRUE(is_success(rec.write_to_file()));
    EXPECT_FALSE(rec.ready_to_write());  // file written exactly once

    auto const frames = read_back(path);
    EXPECT_EQ(frames.size(), 3U);
    for (auto const& got : frames) {
        EXPECT_EQ(got.size(), f.size());
        EXPECT_TRUE(std::equal(got.begin(), got.end(), f.begin()));
    }
}

TEST(tx_pcap_recorder, snaplen_truncates_captured_bytes)
{
    TxPcapRecorder rec;
    std::string const path = "/tmp/sb_txpcap_snap.pcap";
    rec.configure(path, size_t{1} << 20, /*snaplen=*/16, /*duration_ns=*/1'000'000'000ULL);

    auto const f = make_frame(0x20, 200);  // 200 bytes, snaplen 16
    rec.record(f, 1'000'000ULL);
    EXPECT_EQ(rec.frame_count(), 1U);
    // close the window so we can flush
    rec.record(f, 2'000'000'001ULL);
    EXPECT_TRUE(is_success(rec.write_to_file()));

    auto const frames = read_back(path);
    EXPECT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].size(), 16U);  // truncated to snaplen
    EXPECT_TRUE(std::equal(frames[0].begin(), frames[0].end(), f.begin()));

    // The record says the frame was 200 bytes on the wire. Before the fix it
    // said 16, so a snapped capture read as a complete one and the arena's
    // carefully stored original length went nowhere.
    auto const wire = read_back_wire_lengths(path);
    EXPECT_EQ(wire.size(), 1U);
    EXPECT_EQ(wire[0], 200U);
}

TEST(tx_pcap_recorder, arena_full_closes_window_cleanly)
{
    TxPcapRecorder rec;
    std::string const path = "/tmp/sb_txpcap_full.pcap";
    // Cap fits exactly two records: each is REC_HDR(16) + 64 frame bytes = 80.
    rec.configure(path, /*cap_bytes=*/160, /*snaplen=*/1522, /*duration_ns=*/10'000'000'000ULL);

    auto const f = make_frame(0x30, 64);
    rec.record(f, 1'000'000ULL);
    rec.record(f, 1'001'000ULL);
    EXPECT_EQ(rec.frame_count(), 2U);
    EXPECT_TRUE(rec.recording());

    rec.record(f, 1'002'000ULL);  // no room -> closes, dropped
    EXPECT_FALSE(rec.recording());
    EXPECT_TRUE(rec.ready_to_write());
    EXPECT_EQ(rec.frame_count(), 2U);

    EXPECT_TRUE(is_success(rec.write_to_file()));
    EXPECT_EQ(read_back(path).size(), 2U);
}

TEST_MAIN(statusbar_avb_entity, tx_pcap_recorder_test)
