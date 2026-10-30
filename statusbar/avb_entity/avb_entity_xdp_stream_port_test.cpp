// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// XdpStreamPort and StreamPorts tests: the transport words, the port plan and
/// its refusal, the AF_XDP options and UMEM split, the kick health, opening on a
/// missing interface, and (as root, with libxdp) AVTP to and from a veth peer.

#include "statusbar/avb_entity/avb_entity_xdp_stream_port.hpp"

#include "statusbar/avb_entity/avb_entity_stream_ports.hpp"
#include "statusbar/avb_entity/avb_entity_stream_transport.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/net/net_linux_xdp.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/net/net_xdp_avtp_rules.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#if defined(__linux__)
#    include <poll.h>

#    include <linux/if_ether.h>
#    include <linux/if_packet.h>
#    include <linux/magic.h>
#    include <net/if.h>
#    include <sys/socket.h>
#    include <sys/vfs.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

constexpr std::string_view MISSING = "no-such-interface0";

}  // namespace

TEST(avb_entity_stream_transport, words_round_trip_and_others_are_refused)
{
    for (auto const t : {StreamTransport::packet, StreamTransport::xdp, StreamTransport::auto_}) {
        EXPECT_TRUE(parse_stream_transport(stream_transport_name(t)) == t);
    }
    for (auto const m : {StreamXdpMode::auto_, StreamXdpMode::zerocopy, StreamXdpMode::copy}) {
        EXPECT_TRUE(parse_stream_xdp_mode(stream_xdp_mode_name(m)) == m);
    }
    EXPECT_TRUE(stream_transport_name(StreamTransport::auto_) == "auto");
    EXPECT_FALSE(parse_stream_transport("udp").has_value());
    EXPECT_FALSE(parse_stream_xdp_mode("zero-copy").has_value());
}

// xdp drained on its own thread is refused; auto then stays on AF_PACKET.
TEST(avb_entity_stream_ports, the_port_plan_follows_the_transport_and_the_drain_thread)
{
    EXPECT_TRUE(stream_port_plan(StreamTransport::packet, false) == StreamPortPlan::packet);
    EXPECT_TRUE(stream_port_plan(StreamTransport::packet, true) == StreamPortPlan::packet);
    EXPECT_TRUE(stream_port_plan(StreamTransport::xdp, false) == StreamPortPlan::xdp);
    EXPECT_TRUE(stream_port_plan(StreamTransport::xdp, true) == StreamPortPlan::refused);
    EXPECT_TRUE(stream_port_plan(StreamTransport::auto_, false) == StreamPortPlan::xdp_then_packet);
    EXPECT_TRUE(stream_port_plan(StreamTransport::auto_, true) == StreamPortPlan::packet);

    StreamPortsOptions options{.interface_name = MISSING, .transport = StreamTransport::xdp, .rx_on_own_thread = true};
    auto const refused = StreamPorts::check(options);
    EXPECT_TRUE(!refused && refused.error() == std::errc::invalid_argument);
    options.receive = false;  // nothing to receive: nothing refused
    EXPECT_TRUE(StreamPorts::check(options).has_value());
}

TEST(avb_entity_stream_ports, a_refused_receive_opens_nothing)
{
    StreamPorts ports;
    auto const opened = ports.open({.interface_name = MISSING, .transport = StreamTransport::xdp, .rx_on_own_thread = true});
    EXPECT_TRUE(!opened && opened.error() == std::errc::invalid_argument);
    EXPECT_FALSE(ports.tx_error());
    EXPECT_FALSE(ports.rx_error());
    EXPECT_FALSE(ports.xdp_error());
}

TEST(avb_entity_stream_ports, packet_does_not_try_af_xdp)
{
    StreamPorts ports;
    EXPECT_TRUE(ports.open({.interface_name = MISSING}).has_value());
    EXPECT_TRUE(ports.tx_error());
    EXPECT_TRUE(ports.rx_error());
    EXPECT_FALSE(ports.xdp_error());
    EXPECT_TRUE(ports.transport_in_force() == StreamTransport::packet);
    EXPECT_TRUE(&ports.rx() == &ports.packet());
}

// Under xdp the failed AF_XDP open is the transmit and receive error; under
// auto the kit falls back to AF_PACKET, whose own errors are recorded.
TEST(avb_entity_stream_ports, a_failed_af_xdp_open_is_recorded_with_the_fallback_taken)
{
    StreamPorts xdp;
    EXPECT_TRUE(xdp.open({.interface_name = MISSING, .transport = StreamTransport::xdp}).has_value());
    EXPECT_TRUE(xdp.xdp_error());
    EXPECT_TRUE(xdp.rx_error() == xdp.xdp_error());
    EXPECT_TRUE(xdp.tx_error() == xdp.xdp_error());
    EXPECT_TRUE(xdp.transport_in_force() == StreamTransport::packet);

    StreamPorts fallback;
    EXPECT_TRUE(fallback.open({.interface_name = MISSING, .transport = StreamTransport::auto_}).has_value());
    EXPECT_TRUE(fallback.xdp_error());
    EXPECT_TRUE(fallback.rx_error());
    EXPECT_TRUE(fallback.rx_error() != fallback.xdp_error());
    EXPECT_TRUE(&fallback.rx() == &fallback.packet());
    EXPECT_TRUE(&fallback.tx() == &fallback.packet());
    EXPECT_FALSE(fallback.fell_back());  // AF_PACKET did not open either

    // Transmit alone tries AF_XDP too, even with receive on its own thread.
    StreamPorts transmit_only;
    EXPECT_TRUE(
        transmit_only
            .open({.interface_name = MISSING, .receive = false, .transport = StreamTransport::xdp, .rx_on_own_thread = true})
            .has_value());
    EXPECT_TRUE(transmit_only.xdp_error());
    EXPECT_TRUE(transmit_only.tx_error() == transmit_only.xdp_error());
    EXPECT_FALSE(transmit_only.rx_error());
    transmit_only.begin_tick();  // nothing open: nothing to drain
    EXPECT_TRUE(transmit_only.end_tick().has_value());
}

// Eight refused kicks in a row put transmit in error with the last errno; one
// accepted kick clears it, and fewer than eight never set it.
TEST(avb_entity_xdp_stream_port, eight_refused_kicks_in_a_row_are_a_tx_error_until_one_succeeds)
{
    XdpStreamPort::TxHealth health;
    for (uint32_t i = 0; i + 1 < XdpStreamPort::TX_FAILING_AFTER; ++i) {
        health.record(false, EINVAL);
    }
    EXPECT_FALSE(health.failing());
    health.record(true, 0);
    for (uint32_t i = 0; i + 1 < XdpStreamPort::TX_FAILING_AFTER; ++i) {
        health.record(false, EINVAL);
    }
    EXPECT_FALSE(health.failing());
    health.record(false, ENXIO);
    EXPECT_TRUE(health.failing());
    EXPECT_TRUE(health.error() == std::error_code(ENXIO, std::system_category()));
    EXPECT_EQ(health.failures(), uint64_t{2 * XdpStreamPort::TX_FAILING_AFTER - 1});
    health.record(true, 0);
    EXPECT_FALSE(health.failing());
    EXPECT_TRUE(health.error());  // the last refusal is remembered
    health.reset();
    EXPECT_FALSE(health.error());
    EXPECT_EQ(health.failures(), uint64_t{0});
}

TEST(avb_entity_xdp_stream_port, the_options_follow_the_xdp_mode_and_pin_per_interface)
{
    auto const automatic = xdp_stream_port_options(StreamXdpMode::auto_, "enp8s0", 0);
    EXPECT_TRUE(automatic.bind_mode == net::XdpBindMode::auto_);
    EXPECT_TRUE(automatic.attach_mode == net::XdpAttachMode::native);
    EXPECT_TRUE(automatic.pin_path == net::XdpProgram::pin_path_for("enp8s0"));
    auto const zerocopy = xdp_stream_port_options(StreamXdpMode::zerocopy, "enp8s0", 2);
    EXPECT_TRUE(zerocopy.bind_mode == net::XdpBindMode::zero_copy);
    EXPECT_TRUE(zerocopy.attach_mode == net::XdpAttachMode::native);
    EXPECT_EQ(zerocopy.queue_id, uint32_t{2});
    auto const copy = xdp_stream_port_options(StreamXdpMode::copy, "enp8s0", 0);
    EXPECT_TRUE(copy.bind_mode == net::XdpBindMode::copy);
    EXPECT_TRUE(copy.attach_mode == net::XdpAttachMode::generic);
}

TEST(avb_entity_xdp_stream_port, the_config_splits_the_umem_and_stamps_no_frames)
{
    auto const rules = net::avtp_stream_rules();
    auto const options = xdp_stream_port_options(StreamXdpMode::copy, "enp8s0", 1);
    auto const config = XdpStreamPort::config_for("enp8s0", options, rules);
    EXPECT_EQ(config.umem_frame_count, XdpStreamPort::UMEM_FRAMES);
    EXPECT_EQ(config.fill_ring_size, XdpStreamPort::FILL_RING);
    EXPECT_EQ(config.rx_ring_size, XdpStreamPort::RX_RING);
    EXPECT_EQ(config.tx_ring_size, XdpStreamPort::TX_RING);
    EXPECT_EQ(config.queue_id, uint32_t{1});
    EXPECT_EQ(config.rx_clock_id, -1);
    EXPECT_EQ(config.rx_drain_max, size_t{0});
    EXPECT_EQ(config.completion_drain_max, XdpStreamPort::TX_RING);
    EXPECT_EQ(config.filter_rules.size(), size_t{6});
    EXPECT_TRUE(config.default_action == net::XdpAction::pass_kernel);
    EXPECT_TRUE(config.pin_path == options.pin_path);
}

// Launch time needs the driver (netdev tx-launch-time-fifo, asked once by the host) and this build
// (libxdp >= 1.5); the config asks the socket for TX metadata only when the options do.
TEST(avb_entity_xdp_stream_port, launch_time_needs_the_driver_and_the_build)
{
    EXPECT_TRUE(launch_time_support(false).error() == std::errc::operation_not_supported);
    auto const no_query = std::make_error_code(std::errc::no_such_file_or_directory);
    EXPECT_TRUE(launch_time_support(failure(no_query)).error() == no_query);
    bool const built = net::detail::tx_metadata_len(true).has_value();
    auto const supported = launch_time_support(true);
    EXPECT_EQ(supported.has_value(), built);
    EXPECT_TRUE(built || supported.error() == net::XdpError::tx_metadata_not_supported);

    auto const rules = net::avtp_stream_rules();
    auto options = xdp_stream_port_options(StreamXdpMode::zerocopy, "enp1s0", 0);
    EXPECT_FALSE(options.launch_time);
    EXPECT_FALSE(XdpStreamPort::config_for("enp1s0", options, rules).tx_launch_time);
    options.launch_time = true;
    EXPECT_TRUE(XdpStreamPort::config_for("enp1s0", options, rules).tx_launch_time);
}

// Two talker streams on one socket whose departures sit 4 frames apart on the 125 us grid (their
// crossings differ), committed A then B each tick: launch times never decrease (igc's queue is a
// FIFO), A is never held, B waits at most for A, under a packet (6 frames at 48 kHz). An untimed
// frame goes at now, or after the last time given.
TEST(avb_entity_xdp_stream_port, launch_times_never_decrease_across_streams)
{
    constexpr uint64_t FRAME_NS = 20'833;
    constexpr uint64_t TICK_NS = 125'000;
    uint64_t const base = 1'800'000'000ULL * 1'000'000'000ULL;
    XdpStreamPort::LaunchOrder order;
    auto const never = [] { return uint64_t{0}; };
    uint64_t last = 0;
    for (uint64_t k = 0; k < 8; ++k) {
        uint64_t const a = base + (k * TICK_NS) + (4 * FRAME_NS);
        uint64_t const b = base + (k * TICK_NS);
        uint64_t const given_a = order.next(a, never);
        uint64_t const given_b = order.next(b, never);
        EXPECT_EQ(given_a, a);
        EXPECT_TRUE(given_a >= last && given_b >= given_a);
        EXPECT_TRUE(given_b - b < 6 * FRAME_NS);
        last = given_b;
    }
    EXPECT_EQ(order.clamped(), uint64_t{0});  // under a packet: the design's rounding, not counted
    EXPECT_EQ(order.max_clamp_ns(), uint64_t{0});
    EXPECT_EQ(order.next(base + (8 * TICK_NS), never), base + (8 * TICK_NS));
    EXPECT_EQ(order.next(base + (7 * TICK_NS) - 1, never), base + (8 * TICK_NS));  // over a packet: counted
    EXPECT_EQ(order.clamped(), uint64_t{1});
    EXPECT_EQ(order.max_clamp_ns(), TICK_NS + 1);
    uint64_t const later = base + (100 * TICK_NS);
    EXPECT_EQ(order.next(0, [&] { return later; }), later);
    EXPECT_EQ(order.next(0, [&] { return later - 1000; }), later);  // now just behind: the last time
    order.reset();
    EXPECT_EQ(order.next(0, [&] { return base; }), base);
    EXPECT_EQ(order.steps(), uint64_t{0});
}

// A backward PTP step (here 3 s) is not cross-slot rounding: the floor goes at once, counted, so the
// frames after it are not held into the future in igc's FIFO.
TEST(avb_entity_xdp_stream_port, a_backward_clock_step_drops_the_floor)
{
    uint64_t const t = 1'800'000'000ULL * 1'000'000'000ULL;
    auto const never = [] { return uint64_t{0}; };
    XdpStreamPort::LaunchOrder order;
    EXPECT_EQ(order.next(t, never), t);
    EXPECT_EQ(order.next(t - XdpStreamPort::LaunchOrder::STEP_NS, never), t);  // within the bound: held
    EXPECT_EQ(order.next(t - 3'000'000'000ULL, never), t - 3'000'000'000ULL);
    EXPECT_EQ(order.next(t - 3'000'000'000ULL + 125'000, never), t - 3'000'000'000ULL + 125'000);
    EXPECT_EQ(order.steps(), uint64_t{1});
    EXPECT_EQ(order.clamped(), uint64_t{1});  // the STEP_NS hold, over a packet
}

// A kernel before 6.11 refuses the UMEM's TX metadata: probed once before anything is attached, launch
// time goes off with the reason and the port opens once, attaching once; a probe that passes keeps
// it; no launch time asked for, no probe.
TEST(avb_entity_xdp_stream_port, a_kernel_refusing_tx_metadata_is_probed_before_the_one_attach)
{
    int probes = 0;
    auto const refuses = [&probes] {
        ++probes;
        return Status{failure(net::XdpError::tx_metadata_not_supported)};
    };
    auto const passes = [&probes] {
        ++probes;
        return Status{};
    };
    auto options = xdp_stream_port_options(StreamXdpMode::auto_, "enp1s0", 0);
    options.launch_time = true;
    std::error_code why;
    auto const refused = launch_time_as_probed(options, refuses, why);
    EXPECT_TRUE(!refused.launch_time && why == net::XdpError::tx_metadata_not_supported && probes == 1);
    why = {};
    auto const kept = launch_time_as_probed(options, passes, why);
    EXPECT_TRUE(kept.launch_time && !why && probes == 2);
    options.launch_time = false;
    auto const unasked = launch_time_as_probed(options, refuses, why);
    EXPECT_TRUE(!unasked.launch_time && !why && probes == 2);
    // The real probe: refused by a build without TX metadata before the kernel is asked.
    bool const built = net::detail::tx_metadata_len(true).has_value();
    auto const probed = probe_tx_metadata();
    EXPECT_TRUE(built || probed.error() == net::XdpError::tx_metadata_not_supported);
}

// An open asking for launch time on a build (or kernel) without TX metadata records why and goes on without.
TEST(avb_entity_xdp_stream_port, a_refused_launch_time_is_recorded)
{
    XdpStreamPort port;
    auto options = xdp_stream_port_options(StreamXdpMode::auto_, MISSING, 0);
    EXPECT_FALSE(port.open(MISSING, options).has_value());
    EXPECT_FALSE(port.launch_time_error());
    options.launch_time = true;
    EXPECT_FALSE(port.open(MISSING, options).has_value());
    bool const built = net::detail::tx_metadata_len(true).has_value();
    EXPECT_TRUE(built || port.launch_time_error() == net::XdpError::tx_metadata_not_supported);  // built: the kernel's say
    EXPECT_FALSE(port.launch_time());
}

TEST(avb_entity_xdp_stream_port, open_fails_cleanly)
{
    XdpStreamPort port;
    EXPECT_FALSE(port.open(MISSING, xdp_stream_port_options(StreamXdpMode::auto_, MISSING, 0)).has_value());
    EXPECT_FALSE(port.is_open());
    EXPECT_EQ(port.rx_fd(), -1);
    EXPECT_FALSE(port.zero_copy());
    EXPECT_TRUE(port.hardware_address() == ieee::Eui48{});
    EXPECT_FALSE(port.pinned());
    port.begin_cycle();
    EXPECT_EQ(port.tx_pool_size(), size_t{0});
    EXPECT_EQ(port.rx_poll([](std::span<uint8_t const>, std::span<uint8_t const>) {}, 8), size_t{0});
    EXPECT_TRUE(port.end_cycle().has_value());
    EXPECT_TRUE(port.tx_begin(64).empty());
    EXPECT_EQ(port.tx_pool_empty_count(), uint64_t{0});  // closed, not empty
    std::array<uint8_t, 8> const elsewhere{};
    auto const committed = port.tx_commit(elsewhere, ieee::Eui48{}, 2, 3, 0);
    EXPECT_TRUE(!committed && committed.error() == std::errc::invalid_argument);
    EXPECT_TRUE(port.tx_flush().has_value());
    EXPECT_FALSE(port.tx_error());
    EXPECT_FALSE(port.join_multicast(ieee::Eui48{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01}).has_value());
    EXPECT_EQ(port.stats().rx_frames, uint64_t{0});
    EXPECT_EQ(port.stats().tx_errors, uint64_t{1});
    port.close();
}

#if defined(__linux__) && defined(HAVE_XDP)

namespace {

constexpr char const* VETH_TX = "avbxdp0";
constexpr char const* VETH_RX = "avbxdp1";

/// The veth pair avbxdp0/avbxdp1 for one test (root only).
struct Veth
{
    Veth()
    {
        (void)std::system("ip link del avbxdp0 2>/dev/null");
        ok = std::system("ip link add avbxdp0 type veth peer name avbxdp1 && ip link set avbxdp0 up && "
                         "ip link set avbxdp1 up") == 0;
    }
    ~Veth() { (void)std::system("ip link del avbxdp0 2>/dev/null"); }
    Veth(Veth const&) = delete;
    auto operator=(Veth const&) -> Veth& = delete;
    Veth(Veth&&) = delete;
    auto operator=(Veth&&) -> Veth& = delete;
    bool ok{false};
};

/// Whether /proc/net/dev_mcast lists @p group on @p interface.
auto in_dev_mcast(std::string_view const interface, ieee::Eui48 const& group) -> bool
{
    std::string hex;
    for (auto const octet : group.value) {
        hex += std::format("{:02x}", octet);
    }
    std::ifstream mcast{"/proc/net/dev_mcast"};
    for (std::string line; std::getline(mcast, line);) {
        if (line.find(interface) != std::string::npos && line.find(hex) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// An Ethernet frame to @p dest: untagged or under a VID 2 / PCP 3 tag,
/// @p ethertype, then @p payload.
auto make_frame(ieee::Eui48 const& dest, bool const tagged, uint16_t const ethertype, std::span<uint8_t const> const payload)
    -> std::vector<uint8_t>
{
    std::vector<uint8_t> frame(dest.value.begin(), dest.value.end());
    frame.insert(frame.end(), {0x02, 0x00, 0x00, 0x00, 0x00, 0x01});
    if (tagged) {
        frame.insert(frame.end(), {0x81, 0x00, 0x60, 0x02});
    }
    frame.push_back(static_cast<uint8_t>(ethertype >> 8));
    frame.push_back(static_cast<uint8_t>(ethertype & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// A 32-octet AVTPDU of @p subtype whose last octet is @p tag.
auto make_avtpdu(uint8_t const subtype, uint8_t const tag) -> std::array<uint8_t, 32>
{
    std::array<uint8_t, 32> pdu{};
    pdu[0] = subtype;
    pdu[1] = 0x81;  // sv, version 0
    pdu[31] = tag;
    return pdu;
}

struct Received
{
    size_t offset{0};
    bool tagged{false};
    std::vector<uint8_t> payload;
};

}  // namespace

// AAF and CRF, tagged and untagged, reach rx_poll with the AVTPDU at offset 14
// (18 under a tag); ATDECC and non-AVTP frames stay with the kernel. The
// group's membership follows join_multicast / leave_multicast.
TEST(avb_entity_xdp_stream_port, receives_avtp_from_a_veth_peer_when_privileged)
{
    if (::geteuid() != 0) {
        std::println("  skipped: not root");
        return;
    }
    Veth const veth;
    if (!veth.ok) {
        std::println("  skipped: cannot create the veth pair");
        return;
    }
    XdpStreamPort port;
    auto options = xdp_stream_port_options(StreamXdpMode::copy, VETH_RX, 0);
    options.pin_path.clear();  // leave nothing in bpffs
    if (auto opened = port.open(VETH_RX, options); !opened) {
        std::println("  skipped: AF_XDP unavailable ({})", opened.error().message());
        return;
    }
    EXPECT_FALSE(port.zero_copy());
    EXPECT_FALSE(port.pinned());

    ieee::Eui48 const group{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};
    EXPECT_TRUE(port.join_multicast(group).has_value());
    EXPECT_TRUE(in_dev_mcast(VETH_RX, group));

    net::RawnetContext sender;
    if (auto opened = sender.open(VETH_TX, net::ETHERTYPE_AVTP, nullptr, false, /*transmit_only=*/true); !opened) {
        std::println("  skipped: no AF_PACKET socket on {} ({})", VETH_TX, opened.error().message());
        return;
    }
    auto const aaf_untagged = make_avtpdu(0x02, 1);
    auto const aaf_tagged = make_avtpdu(0x02, 2);
    auto const crf_untagged = make_avtpdu(0x04, 3);
    auto const crf_tagged = make_avtpdu(0x04, 4);
    auto const atdecc = make_avtpdu(0xFB, 5);
    auto const other = make_avtpdu(0x02, 6);
    std::array const frames{
        make_frame(group, false, net::ETHERTYPE_AVTP, aaf_untagged),
        make_frame(group, true, net::ETHERTYPE_AVTP, aaf_tagged),
        make_frame(group, false, net::ETHERTYPE_AVTP, crf_untagged),
        make_frame(group, true, net::ETHERTYPE_AVTP, crf_tagged),
        make_frame(group, false, net::ETHERTYPE_AVTP, atdecc),
        make_frame(group, false, 0x88B5, other),
    };
    for (auto const& frame : frames) {
        EXPECT_TRUE(sender.send(frame).has_value());
    }

    std::vector<Received> received;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (received.size() < 4 && std::chrono::steady_clock::now() < deadline) {
        port.begin_cycle();
        (void)port.rx_poll(
            [&received](std::span<uint8_t const> const payload, std::span<uint8_t const> const frame) {
                received.push_back(Received{
                    .offset = static_cast<size_t>(payload.data() - frame.data()),
                    .tagged = frame.size() > 13 && frame[12] == 0x81 && frame[13] == 0x00,
                    .payload = {payload.begin(), payload.end()}});
            },
            16);
        (void)port.end_cycle();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});  // anything that should not arrive
    (void)port.rx_poll(
        [&received](std::span<uint8_t const> const payload, std::span<uint8_t const>) {
            received.push_back(Received{.offset = 0, .tagged = false, .payload = {payload.begin(), payload.end()}});
        },
        16);

    EXPECT_EQ(received.size(), size_t{4});
    EXPECT_EQ(port.stats().rx_frames, uint64_t{4});
    EXPECT_TRUE(port.tx_pool_size() > 0);  // held back from the fill ring
    for (auto const& r : received) {
        EXPECT_EQ(r.offset, r.tagged ? size_t{18} : size_t{14});  // a NIC may strip the tag
        EXPECT_EQ(r.payload.size(), size_t{32});
        if (r.payload.size() == 32) {
            EXPECT_TRUE(r.payload[0] == 0x02 || r.payload[0] == 0x04);
            EXPECT_TRUE(r.payload[31] >= 1 && r.payload[31] <= 4);
        }
    }
    for (uint8_t tag = 1; tag <= 4; ++tag) {
        EXPECT_TRUE(
            std::ranges::any_of(received, [tag](Received const& r) { return r.payload.size() == 32 && r.payload[31] == tag; }));
    }

    EXPECT_TRUE(port.leave_multicast(group).has_value());
    EXPECT_FALSE(in_dev_mcast(VETH_RX, group));
    port.close();
}

// A generic program this port attached is detached at close, its pins with it.
TEST(avb_entity_xdp_stream_port, a_pin_path_pins_the_program_until_close_when_privileged)
{
    if (::geteuid() != 0) {
        std::println("  skipped: not root");
        return;
    }
    Veth const veth;
    if (!veth.ok) {
        std::println("  skipped: cannot create the veth pair");
        return;
    }
    struct statfs fs{};
    bool const bpffs = ::statfs("/sys/fs/bpf", &fs) == 0 && fs.f_type == BPF_FS_MAGIC;
    XdpStreamPort port;
    auto const options = xdp_stream_port_options(StreamXdpMode::copy, VETH_RX, 0);
    if (auto opened = port.open(VETH_RX, options); !opened) {
        std::println("  skipped: AF_XDP unavailable ({})", opened.error().message());
        return;
    }
    EXPECT_EQ(port.pinned(), bpffs);
    std::string const prog_pin = options.pin_path + "/" + std::string{net::XdpProgram::PIN_PROG};
    EXPECT_EQ(::access(prog_pin.c_str(), F_OK) == 0, bpffs);
    port.close();
    EXPECT_FALSE(port.pinned());
    EXPECT_FALSE(::access(prog_pin.c_str(), F_OK) == 0);
    (void)net::XdpProgram::detach(::if_nametoindex(VETH_RX), options.pin_path);
}

namespace {

/// Every AVTP frame arriving on an interface, as on the wire: AF_PACKET with
/// the VLAN tag the stack strips put back from PACKET_AUXDATA (root only).
class Capture
{
  public:
    explicit Capture(char const* const interface)
    {
        fd_ = ::socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_ALL));
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_ALL);
        address.sll_ifindex = static_cast<int>(::if_nametoindex(interface));
        int const one = 1;
        ok_ = fd_ >= 0 && ::bind(fd_, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0 &&
            ::setsockopt(fd_, SOL_PACKET, PACKET_AUXDATA, &one, sizeof(one)) == 0;
    }
    ~Capture()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Capture(Capture const&) = delete;
    auto operator=(Capture const&) -> Capture& = delete;
    Capture(Capture&&) = delete;
    auto operator=(Capture&&) -> Capture& = delete;

    [[nodiscard]] auto ok() const noexcept -> bool { return ok_; }

    /// The next AVTP frame within @p timeout; empty when none came.
    auto next(std::chrono::milliseconds const timeout) -> std::vector<uint8_t>
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd pfd{.fd = fd_, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, 10) <= 0) {
                continue;
            }
            std::array<uint8_t, 2048> buffer{};
            alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(tpacket_auxdata))> control{};
            iovec iov{.iov_base = buffer.data(), .iov_len = buffer.size()};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control.data();
            msg.msg_controllen = control.size();
            auto const length = ::recvmsg(fd_, &msg, 0);
            if (length < 14) {
                continue;
            }
            std::vector<uint8_t> frame(buffer.begin(), buffer.begin() + length);
            for (auto* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_PACKET || c->cmsg_type != PACKET_AUXDATA) {
                    continue;
                }
                tpacket_auxdata aux{};
                std::memcpy(&aux, CMSG_DATA(c), sizeof(aux));
                if ((aux.tp_status & TP_STATUS_VLAN_VALID) != 0) {
                    uint16_t const tpid = (aux.tp_status & TP_STATUS_VLAN_TPID_VALID) != 0 ? aux.tp_vlan_tpid : 0x8100;
                    std::array<uint8_t, 4> const tag{
                        static_cast<uint8_t>(tpid >> 8),
                        static_cast<uint8_t>(tpid),
                        static_cast<uint8_t>(aux.tp_vlan_tci >> 8),
                        static_cast<uint8_t>(aux.tp_vlan_tci)};
                    frame.insert(frame.begin() + 12, tag.begin(), tag.end());
                }
            }
            if (net::avtp_payload_offset(frame)) {
                return frame;
            }
        }
        return {};
    }

  private:
    int fd_{-1};
    bool ok_{false};
};

/// Open @p port in copy mode on @p interface, pinned nowhere; false (with a
/// skip line) when AF_XDP is unavailable.
auto open_copy_port(XdpStreamPort& port, char const* const interface) -> bool
{
    auto options = xdp_stream_port_options(StreamXdpMode::copy, interface, 0);
    options.pin_path.clear();  // leave nothing in bpffs
    if (auto opened = port.open(interface, options); !opened) {
        std::println("  skipped: AF_XDP unavailable ({})", opened.error().message());
        return false;
    }
    return true;
}

ieee::Eui48 const TX_DEST{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x07};

/// A talker with an AAF output (index 0) and a Milan CRF output (index 1) on @p port.
struct TestTalker
{
    explicit TestTalker(StreamPort& port)
    {
        auto const spec = [](uint16_t const index, uint64_t const word) {
            StreamSpec s{};
            s.index = index;
            s.format_word = word;
            s.format = decode_stream_format(word);
            return s;
        };
        tsn::StreamId const id{ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x07}, 7};
        ok = talker.open_stream(spec(0, 0x020702200200C000ULL), id, TX_DEST).has_value() &&
            talker.open_stream(spec(1, 0x041060010000BB80ULL), id, TX_DEST).has_value() && talker.open_tx(port).has_value();
    }
    /// One AAF and one CRF frame; true when both were committed.
    auto send(uint64_t const n) -> bool
    {
        StreamRawRenderFn const render = [](std::span<uint8_t> payload, uint32_t, uint64_t first_index, uint64_t) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<uint8_t>(first_index + i);
            }
            return true;
        };
        std::array<uint64_t, 1> const ts{0x0123'4567'0000'0000ULL + n};
        bool const aaf = talker.transmit_aaf_raw(*talker.slot_for(0), n * 12, 1'000'000 + (n * 125'000), 12, render);
        bool const crf = talker.transmit_crf_raw(*talker.slot_for(1), ts);
        return aaf && crf;
    }

    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<uint64_t> last_gptp_ns{0};
    TalkerStreams talker{
        TalkerStreamsConfig{.sample_rate = 96'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    bool ok{false};
};

/// Kicks and skipped kicks: one per tx_flush.
auto flush_calls(XdpStreamPort const& port) -> uint64_t
{
    return port.socket_stats().tx_flush_count + port.socket_stats().tx_flush_skipped;
}

/// Run cycles until @p port's TX pool is full again (completions reaped).
auto await_full_pool(XdpStreamPort& port) -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (std::chrono::steady_clock::now() < deadline) {
        (void)port.tx_flush();
        port.begin_cycle();
        (void)port.end_cycle();
        if (port.tx_pool_size() == XdpStreamPort::TX_POOL) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return false;
}

}  // namespace

// The AAF and CRF frames an XdpStreamPort transmits arrive at the veth peer
// byte for byte as the PacketStreamPort sends them for the same input; the tap
// sees each, and tx_flush is one kick (or skipped kick) per call.
TEST(avb_entity_xdp_stream_port, transmits_the_frames_the_packet_port_sends_when_privileged)
{
    if (::geteuid() != 0) {
        std::println("  skipped: not root");
        return;
    }
    Veth const veth;
    if (!veth.ok) {
        std::println("  skipped: cannot create the veth pair");
        return;
    }
    XdpStreamPort xdp;
    if (!open_copy_port(xdp, VETH_TX)) {
        return;
    }
    PacketStreamPort packet;
    EXPECT_TRUE(packet.open_tx(VETH_TX, 3).has_value());
    Capture capture{VETH_RX};
    EXPECT_TRUE(capture.ok());
    std::vector<std::vector<uint8_t>> tapped;
    xdp.set_tx_tap([&tapped](std::span<uint8_t const> const frame) { tapped.emplace_back(frame.begin(), frame.end()); });

    TestTalker over_xdp{xdp};
    TestTalker over_packet{packet};
    EXPECT_TRUE(over_xdp.ok && over_packet.ok);
    for (uint64_t n = 0; n < 3; ++n) {
        xdp.begin_cycle();
        EXPECT_TRUE(over_xdp.send(n));
        uint64_t const before = flush_calls(xdp);
        EXPECT_TRUE(over_xdp.talker.flush().has_value());
        EXPECT_EQ(flush_calls(xdp), before + 1);
        EXPECT_TRUE(xdp.end_cycle().has_value());
        auto const xdp_aaf = capture.next(std::chrono::milliseconds{500});
        auto const xdp_crf = capture.next(std::chrono::milliseconds{500});

        EXPECT_TRUE(over_packet.send(n));
        auto const packet_aaf = capture.next(std::chrono::milliseconds{500});
        auto const packet_crf = capture.next(std::chrono::milliseconds{500});

        EXPECT_FALSE(xdp_aaf.empty());
        EXPECT_FALSE(xdp_crf.empty());
        EXPECT_TRUE(xdp_aaf == packet_aaf);
        EXPECT_TRUE(xdp_crf == packet_crf);
        EXPECT_EQ(xdp_aaf.size(), size_t{18 + 24 + (12 * 8 * 4)});
        EXPECT_EQ(tapped.size(), size_t{2 * (n + 1)});
        if (tapped.size() == 2 * (n + 1)) {
            EXPECT_TRUE(tapped[2 * n] == xdp_aaf);
            EXPECT_TRUE(tapped[(2 * n) + 1] == xdp_crf);
        }
    }
    EXPECT_EQ(xdp.stats().tx_frames, uint64_t{6});
    EXPECT_EQ(xdp.stats().tx_errors, uint64_t{0});
    EXPECT_FALSE(xdp.tx_error());

    // Nothing outstanding: the flush is a skipped kick.
    EXPECT_TRUE(await_full_pool(xdp));
    uint64_t const skipped = xdp.socket_stats().tx_flush_skipped;
    EXPECT_TRUE(xdp.tx_flush().has_value());
    EXPECT_EQ(xdp.socket_stats().tx_flush_skipped, skipped + 1);
    xdp.close();
    packet.close();
}

// tx_begin reuses one pooled frame until committed; a completion returns it. An empty pool or a
// full ring refuses the frame without taking a sequence number, and a full ring keeps it pooled.
TEST(avb_entity_xdp_stream_port, the_tx_pool_accounts_for_every_frame_when_privileged)
{
    if (::geteuid() != 0) {
        std::println("  skipped: not root");
        return;
    }
    Veth const veth;
    if (!veth.ok) {
        std::println("  skipped: cannot create the veth pair");
        return;
    }
    XdpStreamPort port;
    if (!open_copy_port(port, VETH_TX)) {
        return;
    }
    size_t tapped = 0;
    port.set_tx_tap([&tapped](std::span<uint8_t const>) { ++tapped; });
    port.begin_cycle();
    EXPECT_EQ(port.tx_pool_size(), XdpStreamPort::TX_POOL);
    EXPECT_TRUE(port.tx_begin(StreamPort::MAX_TX_PAYLOAD + 1).empty());
    EXPECT_EQ(port.tx_pool_size(), XdpStreamPort::TX_POOL);

    auto const first = port.tx_begin(64);
    EXPECT_EQ(first.size(), size_t{64});
    EXPECT_EQ(port.tx_pool_size(), XdpStreamPort::TX_POOL - 1);
    auto const again = port.tx_begin(128);
    EXPECT_TRUE(again.data() == first.data());  // not committed: the same frame
    EXPECT_EQ(port.tx_pool_size(), XdpStreamPort::TX_POOL - 1);
    std::array<uint8_t, 8> const elsewhere{};
    auto const stray = port.tx_commit(elsewhere, TX_DEST, 2, 3, 0);
    EXPECT_TRUE(!stray && stray.error() == std::errc::invalid_argument);
    auto const too_long = port.tx_commit(std::span<uint8_t const>{again.data(), again.size() + 1}, TX_DEST, 2, 3, 0);
    EXPECT_TRUE(!too_long && too_long.error() == std::errc::invalid_argument);
    std::ranges::fill(again, uint8_t{0x02});
    EXPECT_TRUE(port.tx_commit(again.first(46), TX_DEST, 2, 3, 0).has_value());
    EXPECT_EQ(port.tx_pool_size(), XdpStreamPort::TX_POOL - 1);  // consumed
    EXPECT_TRUE(await_full_pool(port));                          // completed, back

    // Empty the pool: the talker is refused and its sequence numbers stay.
    auto const commit_one = [&port]() {
        auto const window = port.tx_begin(46);
        if (window.empty()) {
            return false;
        }
        std::ranges::fill(window, uint8_t{0x02});
        return port.tx_commit(window, TX_DEST, 2, 3, 0).has_value();
    };
    TestTalker talker{port};
    EXPECT_TRUE(talker.ok);
    port.begin_cycle();
    size_t committed = 0;
    while (commit_one()) {
        ++committed;
    }
    EXPECT_EQ(committed, XdpStreamPort::TX_POOL);
    EXPECT_EQ(port.tx_pool_empty_count(), uint64_t{1});
    EXPECT_FALSE(talker.send(0));
    EXPECT_EQ(talker.talker.slot_for(0)->aaf->sequence_num, uint8_t{0});
    EXPECT_EQ(talker.talker.slot_for(1)->crf->sequence_num, uint8_t{0});

    // Without a kick copy mode sends nothing: a second pool's worth fills the
    // TX ring, and the next commit finds it full.
    port.begin_cycle();
    EXPECT_EQ(port.tx_pool_size(), XdpStreamPort::TX_POOL);
    for (size_t i = 0; i < XdpStreamPort::TX_RING - XdpStreamPort::TX_POOL; ++i) {
        EXPECT_TRUE(commit_one());
    }
    port.begin_cycle();
    size_t const pool = port.tx_pool_size();
    EXPECT_TRUE(pool > 0);
    uint64_t const errors = port.stats().tx_errors;
    EXPECT_FALSE(talker.send(1));
    EXPECT_EQ(port.socket_stats().tx_ring_full_count, uint64_t{2});  // the AAF and the CRF frame
    EXPECT_EQ(port.stats().tx_errors, errors + 2);
    EXPECT_EQ(port.tx_pool_size(), pool);  // both frames back in the pool
    EXPECT_EQ(talker.talker.slot_for(0)->aaf->sequence_num, uint8_t{0});
    EXPECT_EQ(talker.talker.slot_for(1)->crf->sequence_num, uint8_t{0});
    EXPECT_EQ(talker.talker.slot_for(0)->tx_packets, uint64_t{0});

    // Kicked, the ring drains and every frame comes back.
    EXPECT_TRUE(await_full_pool(port));
    EXPECT_TRUE(talker.send(2));
    EXPECT_EQ(talker.talker.slot_for(0)->aaf->sequence_num, uint8_t{1});
    EXPECT_EQ(tapped, size_t{1 + XdpStreamPort::TX_RING + 2});
    EXPECT_FALSE(port.tx_error());
    port.close();
    EXPECT_EQ(port.tx_pool_size(), size_t{0});
}

#endif  // __linux__ && HAVE_XDP

TEST_MAIN(statusbar_avb_entity, avb_entity_xdp_stream_port_test)
