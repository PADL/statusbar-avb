# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  udptun
  SOURCES
  udptun_identity.cpp
  udptun_per_source_tracker.cpp
  udptun_redundant_rx.cpp
  udptun_report.cpp
  udptun_session_helpers.cpp
  udptun_stats.cpp
  udptun_version.cpp
  udptun_wire_clock.cpp
  udptun_csv_record.cpp
  DEPS
  statusbar::avtp
  statusbar::csv
  statusbar::colbin
  statusbar::buffer
  statusbar::container
  statusbar::gptp
  statusbar::ieee
  statusbar::net
  statusbar::ptpclient
  statusbar::stats
  statusbar::status
  statusbar::itc
  statusbar::tsn
  TESTS
  udptun_deadline_timer_test.cpp
  udptun_monotonic_ring_test.cpp
  udptun_per_source_tracker_test.cpp
  udptun_redundant_rx_test.cpp
  udptun_redundant_tx_test.cpp
  udptun_reflect_test.cpp
  udptun_report_test.cpp
  udptun_stats_test.cpp
  udptun_tx_history_test.cpp
  udptun_wire_clock_test.cpp
  udptun_csv_record_test.cpp
  udptun_record_sink_test.cpp
  udptun_aaf_v1_codec_test.cpp
  udptun_audio_reframe_test.cpp
  udptun_audio_ingest_test.cpp
  udptun_audio_egress_test.cpp
  udptun_session_helpers_test.cpp)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults ON for Clang).
# The AAF-v1/AnnexJ codec parses WAN-untrusted inter-site tunnel datagrams.
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/udptun_aaf_v1_codec_fuzzer.cpp)
