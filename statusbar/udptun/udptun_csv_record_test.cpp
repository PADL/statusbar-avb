// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_csv_record.hpp"

#include "statusbar/test/test.hpp"

#include <array>
#include <string_view>

using namespace statusbar;

TEST(udptun_csv_record, role_to_string_covers_all_enumerators)
{
    EXPECT_EQ(udptun::role_to_string(udptun::PacketRole::SelfPrimary), std::string_view{"self_primary"});
    EXPECT_EQ(udptun::role_to_string(udptun::PacketRole::SelfRedundant), std::string_view{"self_redundant"});
    EXPECT_EQ(udptun::role_to_string(udptun::PacketRole::SelfLegacy), std::string_view{"self_legacy"});
    EXPECT_EQ(udptun::role_to_string(udptun::PacketRole::RemotePrimary), std::string_view{"remote_primary"});
    EXPECT_EQ(udptun::role_to_string(udptun::PacketRole::RemoteRedundant), std::string_view{"remote_redundant"});
    EXPECT_EQ(udptun::role_to_string(udptun::PacketRole::RemoteLegacy), std::string_view{"remote_legacy"});
}

TEST(udptun_csv_record, format_produces_expected_fields)
{
    udptun::UdpTunCsvRecord rec{};
    rec.rx_gptp_ns = 1778451039989000123LL;
    rec.presentation_time_ns = 1778451039987000000LL;
    rec.latency_ns = 2003123;
    rec.sender_id = ieee::Eui64{0x02, 0x00, 0x00, 0x00, 0x00, 0xdf, 0xd1, 0x96};
    rec.sequence = 42;
    rec.interval_us = 1000;
    rec.role = static_cast<uint8_t>(udptun::PacketRole::RemotePrimary);

    udptun::CsvScratch scratch{};
    std::array<std::string_view, 7> out{};
    udptun::format_csv_record(rec, scratch, out);

    EXPECT_EQ(out[0], std::string_view{"1778451039989000123"});
    EXPECT_EQ(out[1], std::string_view{"1778451039987000000"});
    EXPECT_EQ(out[2], std::string_view{"2003123"});
    EXPECT_EQ(out[3], std::string_view{"02:00:00:00:00:df:d1:96"});
    EXPECT_EQ(out[4], std::string_view{"42"});
    EXPECT_EQ(out[5], std::string_view{"1000"});
    EXPECT_EQ(out[6], std::string_view{"remote_primary"});
}

TEST(udptun_csv_record, header_matches_format_field_count)
{
    EXPECT_EQ(udptun::kUdpTunCsvHeader.size(), size_t{7});
    EXPECT_EQ(udptun::kUdpTunCsvHeader[0], std::string_view{"rx_gptp_ns"});
    EXPECT_EQ(udptun::kUdpTunCsvHeader[6], std::string_view{"role"});
}

TEST(udptun_csv_record_sink, append_is_noop_when_ring_null)
{
    udptun::CsvSink sink{};
    udptun::UdpTunCsvRecord rec{};
    sink.append(rec);  // must not crash
    EXPECT_TRUE(sink.ring == nullptr);
}

TEST(udptun_csv_record_sink, append_publishes_until_ring_full_then_drops)
{
    udptun::CsvRing ring{};
    itc::TelemetryCounter<uint64_t> dropped{};
    udptun::CsvSink sink{.ring = &ring, .records_dropped = &dropped};

    // QueuedPipe of capacity N holds at most N-1 entries before
    // try_publish starts failing (one slot is the empty/full sentinel).
    size_t const usable = udptun::csv_ring_capacity - 1;
    udptun::UdpTunCsvRecord rec{};
    for (size_t i = 0; i < usable; ++i) {
        sink.append(rec);
    }
    EXPECT_EQ(dropped.load(), uint64_t{0});
    sink.append(rec);  // one beyond capacity → drop
    EXPECT_EQ(dropped.load(), uint64_t{1});
}

TEST_MAIN(statusbar_udptun, udptun_csv_record_test)
