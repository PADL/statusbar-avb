#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The TX-side figures of the live report: how many packets were sent, how
/// many of them are old enough to have come back, and what the self-loopback
/// redundancy tracker made of the ones that did.
///
/// They come from the TX history and the redundancy tracker. The thread that
/// runs the session's packet work owns both and writes them on every packet,
/// so the reporter thread must not walk either while that happens: the
/// history's ring would be read mid-write, and the tracker's scan retires
/// slots the receive path is filling. Instead the owning thread takes the
/// snapshot (take_loss_snapshot), publishes it whole through a triple
/// buffer, and the reporter prints the published copy.

#include "statusbar/udptun/udptun_redundant_rx.hpp"
#include "statusbar/udptun/udptun_tx_history.hpp"

#include <cstdint>

namespace statusbar::udptun {

struct LossSnapshot
{
    uint64_t sent_total{0};
    /// Packets sent by `now - grace`: old enough that a non-arrival is loss,
    /// not pipeline depth (see TxHistory).
    uint64_t sent_at_cutoff{0};
    uint64_t recovered{0};
    uint64_t true_loss{0};
    uint64_t primary_received{0};
};

/// Retire the redundancy slots more than @p grace_ns behind @p redundancy_ref_pt_ns
/// and take the figures as of @p now_mraw. Called on the thread that owns
/// @p history and @p redundancy.
[[nodiscard]] inline auto take_loss_snapshot(
    TxHistory const& history,
    RedundantRxTracker& redundancy,
    uint64_t sent_total,
    int64_t now_mraw,
    int64_t grace_ns,
    int64_t redundancy_ref_pt_ns) noexcept -> LossSnapshot
{
    redundancy.scan(redundancy_ref_pt_ns, grace_ns);
    return LossSnapshot{
        .sent_total = sent_total,
        .sent_at_cutoff = history.sent_count_at_or_before(now_mraw - grace_ns),
        .recovered = redundancy.recovered(),
        .true_loss = redundancy.true_loss(),
        .primary_received = redundancy.primary_received(),
    };
}

}  // namespace statusbar::udptun
