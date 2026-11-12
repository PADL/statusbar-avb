#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// **udptun** — UDP-tunneled measurement and streaming framework.
///
/// Generic scaffolding for sending and receiving sequence-numbered,
/// gPTP-timestamped UDP datagrams with per-source latency / loss
/// tracking, optional dual-stream temporal-decorrelation redundancy,
/// and an opt-in stateless reflector for round-trip latency. The wire
/// packet format is a customization point: callers supply a Codec type
/// that knows how to encode/decode bytes ↔ {sender_id, sequence,
/// tx_gptp_ns, app_data}, classify primary vs redundant copies, and
/// validate received bytes for the reflector.
///
/// Two codecs are anticipated:
///
///   - **OwlmCodec** — the 32-byte OWLM measurement header. Existing
///     owlm_tool runs on top of `Session<OwlmCodec>`.
///   - **AafV1Codec** — IEEE 1722-2025 AAF version 1 carried over
///     IEEE 1722 Annex J UDP encapsulation. The Annex J 32-bit outer
///     sequence drives udptun's transport-level loss/redundancy
///     tracking; the AAF v1 inner header carries audio metadata
///     (per-stream sequence, 64-bit avtp_timestamp, grandmaster id).
///
/// See docs/UDPTUN.md for the framework architecture.

#include "statusbar/udptun/udptun_codec_concept.hpp"
#include "statusbar/udptun/udptun_csv_record.hpp"
#include "statusbar/udptun/udptun_deadline_timer.hpp"
#include "statusbar/udptun/udptun_identity.hpp"
#include "statusbar/udptun/udptun_loss_snapshot.hpp"
#include "statusbar/udptun/udptun_monotonic_ring.hpp"
#include "statusbar/udptun/udptun_per_source_tracker.hpp"
#include "statusbar/udptun/udptun_redundant_rx.hpp"
#include "statusbar/udptun/udptun_redundant_tx.hpp"
#include "statusbar/udptun/udptun_reflect.hpp"
#include "statusbar/udptun/udptun_report.hpp"
#include "statusbar/udptun/udptun_session.hpp"
#include "statusbar/udptun/udptun_session_helpers.hpp"
#include "statusbar/udptun/udptun_stats.hpp"
#include "statusbar/udptun/udptun_time_source.hpp"
#include "statusbar/udptun/udptun_tx_history.hpp"
#include "statusbar/udptun/udptun_version.hpp"
#include "statusbar/udptun/udptun_wire_clock.hpp"

namespace statusbar::udptun {}
