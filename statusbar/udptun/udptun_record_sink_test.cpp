// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_record_sink.hpp"

#include "statusbar/colbin/colbin_reader.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/udptun/udptun_csv_record.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace statusbar;

namespace {

// RAII scratch path in the system temp dir; removed on destruction.
struct ScratchPath
{
    std::filesystem::path path;
    explicit ScratchPath(char const* suffix)
        : path(std::filesystem::temp_directory_path() / make_name(suffix))
    {}
    ~ScratchPath() noexcept
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ScratchPath(ScratchPath const&) = delete;
    auto operator=(ScratchPath const&) -> ScratchPath& = delete;

    static auto make_name(char const* suffix) -> std::string
    {
        std::array<char, 96> buf{};
        std::snprintf(buf.data(), buf.size(), "udptun_record_sink_%d_%d_%s", ::getpid(), std::rand(), suffix);
        return buf.data();
    }
};

auto make_record(uint32_t seq) -> udptun::UdpTunCsvRecord
{
    udptun::UdpTunCsvRecord rec{};
    rec.rx_gptp_ns = 1'778'451'039'989'000'000LL + seq;
    rec.presentation_time_ns = 1'778'451'039'987'000'000LL;
    rec.latency_ns = 2'003'123;
    rec.sender_id = ieee::Eui64{0x02, 0x00, 0x00, 0x00, 0x00, 0xdf, 0xd1, 0x96};
    rec.sequence = seq;
    rec.interval_us = 1000;
    rec.role = static_cast<uint8_t>(udptun::PacketRole::RemotePrimary);
    return rec;
}

auto read_lines(std::filesystem::path const& path) -> std::vector<std::string>
{
    std::ifstream in{path};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {  // CsvWriter emits CRLF
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

TEST(udptun_record_sink, disabled_when_no_paths)
{
    udptun::RecordSink rs{"", "", udptun::default_colbin_capacity_bytes};
    EXPECT_FALSE(rs.enabled());
    // The append handle is inert: null ring means the RX hot path does nothing.
    auto const handle = rs.sink();
    EXPECT_EQ(handle.ring, nullptr);
    EXPECT_EQ(handle.records_dropped, nullptr);
    handle.append(make_record(0));  // no-op, must not crash
    EXPECT_EQ(rs.records_dropped(), 0U);
    EXPECT_TRUE(static_cast<bool>(rs.flush()));  // success on a disabled sink
}

TEST(udptun_record_sink, csv_rows_written_in_order)
{
    ScratchPath csv{"rows.csv"};
    constexpr uint32_t kCount = 5;
    {
        udptun::RecordSink rs{csv.path.string(), "", udptun::default_colbin_capacity_bytes};
        EXPECT_TRUE(rs.enabled());
        auto const handle = rs.sink();
        EXPECT_NE(handle.ring, nullptr);
        for (uint32_t i = 0; i < kCount; ++i) {
            handle.append(make_record(i));
        }
        EXPECT_TRUE(static_cast<bool>(rs.flush()));  // stops the writer, flushes
    }

    auto const lines = read_lines(csv.path);
    // header + kCount data rows.
    EXPECT_EQ(lines.size(), static_cast<size_t>(kCount) + 1);
    EXPECT_EQ(lines[0], std::string{"rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,sequence,interval_us,role"});
    // Rows preserve publish order; spot-check the sequence column (index 4).
    for (uint32_t i = 0; i < kCount; ++i) {
        std::stringstream ss{lines[i + 1]};
        std::string field;
        std::vector<std::string> cols;
        while (std::getline(ss, field, ',')) {
            cols.push_back(field);
        }
        EXPECT_EQ(cols.size(), 7U);
        EXPECT_EQ(cols[4], std::to_string(i));
        EXPECT_EQ(cols[6], std::string{"remote_primary"});
    }
}

TEST(udptun_record_sink, colbin_rows_written)
{
    ScratchPath bin{"rows.colbin"};
    constexpr uint32_t kCount = 8;
    {
        udptun::RecordSink rs{"", bin.path.string(), udptun::default_colbin_capacity_bytes};
        EXPECT_TRUE(rs.enabled());
        auto const handle = rs.sink();
        for (uint32_t i = 0; i < kCount; ++i) {
            handle.append(make_record(i));
        }
        EXPECT_TRUE(static_cast<bool>(rs.flush()));
    }

    auto reader = colbin::Reader::open(bin.path);
    EXPECT_TRUE(static_cast<bool>(reader));
    EXPECT_EQ(reader->row_count(), static_cast<uint64_t>(kCount));
}

TEST(udptun_record_sink, csv_and_colbin_together)
{
    ScratchPath csv{"both.csv"};
    ScratchPath bin{"both.colbin"};
    constexpr uint32_t kCount = 4;
    {
        udptun::RecordSink rs{csv.path.string(), bin.path.string(), udptun::default_colbin_capacity_bytes};
        EXPECT_TRUE(rs.enabled());
        auto const handle = rs.sink();
        for (uint32_t i = 0; i < kCount; ++i) {
            handle.append(make_record(i));
        }
        EXPECT_TRUE(static_cast<bool>(rs.flush()));
    }
    EXPECT_EQ(read_lines(csv.path).size(), static_cast<size_t>(kCount) + 1);
    auto reader = colbin::Reader::open(bin.path);
    EXPECT_TRUE(static_cast<bool>(reader));
    EXPECT_EQ(reader->row_count(), static_cast<uint64_t>(kCount));
}

TEST(udptun_record_sink, destructor_stops_without_flush)
{
    // No explicit flush() — ~RecordSink must still stop the thread and drain the ring.
    ScratchPath csv{"dtor.csv"};
    constexpr uint32_t kCount = 3;
    {
        udptun::RecordSink rs{csv.path.string(), "", udptun::default_colbin_capacity_bytes};
        auto const handle = rs.sink();
        for (uint32_t i = 0; i < kCount; ++i) {
            handle.append(make_record(i));
        }
    }  // destructor runs stop() -> joins writer -> final flush
    EXPECT_EQ(read_lines(csv.path).size(), static_cast<size_t>(kCount) + 1);
}

TEST_MAIN(statusbar_udptun, udptun_record_sink_test)
