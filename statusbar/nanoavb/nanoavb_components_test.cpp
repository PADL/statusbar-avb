// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
//
// GptpAnnounceHandler: the grandmaster in force, as the Announces name it and
// as their silence takes it away.

#include "statusbar/nanoavb/nanoavb_components.hpp"

#include "statusbar/gptp/gptp.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <optional>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;

namespace {

constexpr int64_t ONE_SECOND_NS = 1'000'000'000;

auto announce_from(gptp::ClockIdentity const& gm, int8_t const log_interval = 0) -> gptp::AnnounceMessage
{
    gptp::AnnounceMessage ann{};
    ann.init(1);
    ann.grandmaster_identity = gm;
    ann.header.log_message_interval = static_cast<uint8_t>(log_interval);
    return ann;
}

/// What the callbacks were told. The handler under test is on no interface:
/// its socket never opens, observe_announce() and tick() work all the same.
struct Observed
{
    std::vector<gptp::ClockIdentity> changes;
    std::optional<gptp::ClockIdentity> last_announce_gm_at_change;
    int losses{0};
};

gptp::ClockIdentity const kGmA{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04};

}  // namespace

TEST(gptp_announce_handler, a_callback_sees_the_announce_that_named_the_grandmaster)
{
    Observed seen;
    GptpAnnounceHandler handler{"no-such-interface-0"};
    handler.set_callbacks(GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&](int64_t, gptp::ClockIdentity const& gm, gptp::AnnounceMessage const&) {
                seen.changes.push_back(gm);
                if (auto const* last = handler.last_announce()) {
                    seen.last_announce_gm_at_change = last->grandmaster_identity;
                }
            },
        .grandmaster_lost = [&](int64_t) { ++seen.losses; }});
    EXPECT_FALSE(handler.has_grandmaster());

    handler.observe_announce(ONE_SECOND_NS, announce_from(kGmA));
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(seen.changes.size(), size_t{1});
    EXPECT_TRUE(seen.last_announce_gm_at_change.has_value());
    EXPECT_TRUE(seen.last_announce_gm_at_change.has_value() && *seen.last_announce_gm_at_change == kGmA);

    // The same grandmaster again: nothing to report.
    handler.observe_announce(2 * ONE_SECOND_NS, announce_from(kGmA));
    EXPECT_EQ(seen.changes.size(), size_t{1});
    EXPECT_EQ(seen.losses, 0);
}

TEST(gptp_announce_handler, silence_for_the_receipt_timeout_loses_the_grandmaster)
{
    Observed seen;
    GptpAnnounceHandler handler{"no-such-interface-0"};
    handler.set_callbacks(GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&](int64_t, gptp::ClockIdentity const& gm, gptp::AnnounceMessage const&) { seen.changes.push_back(gm); },
        .grandmaster_lost = [&](int64_t) { ++seen.losses; }});

    // logMessageInterval 0: one-second Announces, held for three.
    handler.observe_announce(10 * ONE_SECOND_NS, announce_from(kGmA, 0));
    EXPECT_EQ(handler.announce_receipt_timeout_ns(), 3 * ONE_SECOND_NS);
    handler.tick((13 * ONE_SECOND_NS) - 1);
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(seen.losses, 0);
    handler.tick(13 * ONE_SECOND_NS);
    EXPECT_FALSE(handler.has_grandmaster());
    EXPECT_EQ(seen.losses, 1);
    // The last Announce is still readable (its domain, for the reports).
    EXPECT_TRUE(handler.last_announce() != nullptr);
    // Silence after the loss reports nothing more.
    handler.tick(20 * ONE_SECOND_NS);
    EXPECT_EQ(seen.losses, 1);

    // Announces return: the grandmaster is in force again, as a change.
    handler.observe_announce(21 * ONE_SECOND_NS, announce_from(kGmA, -3));
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(seen.changes.size(), size_t{2});
    // logMessageInterval -3: 125 ms Announces, held for 375 ms.
    EXPECT_EQ(handler.announce_receipt_timeout_ns(), (3 * ONE_SECOND_NS) / 8);
}

TEST_MAIN(statusbar_nanoavb, nanoavb_components_test)
