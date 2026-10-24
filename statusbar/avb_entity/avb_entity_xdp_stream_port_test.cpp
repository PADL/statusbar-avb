// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// XdpStreamPort and StreamPorts tests: the transport words, the receive plan
/// and its refusal, the AF_XDP options and UMEM split, opening on a missing
/// interface, and (as root, with libxdp) AVTP received from a veth peer.

#include "statusbar/avb_entity/avb_entity_xdp_stream_port.hpp"

#include "statusbar/avb_entity/avb_entity_stream_ports.hpp"
#include "statusbar/avb_entity/avb_entity_stream_transport.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/net/net_xdp_avtp_rules.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
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
TEST(avb_entity_stream_ports, the_receive_plan_follows_the_transport_and_the_drain_thread)
{
    EXPECT_TRUE(stream_rx_plan(StreamTransport::packet, false) == StreamRxPlan::packet);
    EXPECT_TRUE(stream_rx_plan(StreamTransport::packet, true) == StreamRxPlan::packet);
    EXPECT_TRUE(stream_rx_plan(StreamTransport::xdp, false) == StreamRxPlan::xdp);
    EXPECT_TRUE(stream_rx_plan(StreamTransport::xdp, true) == StreamRxPlan::refused);
    EXPECT_TRUE(stream_rx_plan(StreamTransport::auto_, false) == StreamRxPlan::xdp_then_packet);
    EXPECT_TRUE(stream_rx_plan(StreamTransport::auto_, true) == StreamRxPlan::packet);

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

// Under xdp the failed AF_XDP open is the receive error; under auto the kit
// falls back to AF_PACKET, whose own error is the receive error.
TEST(avb_entity_stream_ports, a_failed_af_xdp_open_is_recorded_with_the_fallback_taken)
{
    StreamPorts xdp;
    EXPECT_TRUE(xdp.open({.interface_name = MISSING, .transport = StreamTransport::xdp}).has_value());
    EXPECT_TRUE(xdp.xdp_error());
    EXPECT_TRUE(xdp.rx_error() == xdp.xdp_error());
    EXPECT_TRUE(xdp.transport_in_force() == StreamTransport::packet);

    StreamPorts fallback;
    EXPECT_TRUE(fallback.open({.interface_name = MISSING, .transport = StreamTransport::auto_}).has_value());
    EXPECT_TRUE(fallback.xdp_error());
    EXPECT_TRUE(fallback.rx_error());
    EXPECT_TRUE(fallback.rx_error() != fallback.xdp_error());
    EXPECT_TRUE(&fallback.rx() == &fallback.packet());

    StreamPorts transmit_only;
    EXPECT_TRUE(transmit_only.open({.interface_name = MISSING, .receive = false, .transport = StreamTransport::xdp}).has_value());
    EXPECT_FALSE(transmit_only.xdp_error());  // AF_XDP receives only, for now
    EXPECT_FALSE(transmit_only.rx_error());
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
    EXPECT_EQ(config.filter_rules.size(), size_t{6});
    EXPECT_TRUE(config.default_action == net::XdpAction::pass_kernel);
    EXPECT_TRUE(config.pin_path == options.pin_path);
}

TEST(avb_entity_xdp_stream_port, open_fails_cleanly)
{
    XdpStreamPort port;
    EXPECT_FALSE(port.open(MISSING, xdp_stream_port_options(StreamXdpMode::auto_, MISSING, 0)).has_value());
    EXPECT_FALSE(port.is_open());
    EXPECT_EQ(port.rx_fd(), -1);
    EXPECT_FALSE(port.zero_copy());
    EXPECT_TRUE(port.hardware_address() == ieee::Eui48{});
    port.begin_cycle();
    EXPECT_EQ(port.tx_pool_size(), size_t{0});
    EXPECT_EQ(port.rx_poll([](std::span<uint8_t const>, std::span<uint8_t const>) {}, 8), size_t{0});
    EXPECT_TRUE(port.end_cycle().has_value());
    EXPECT_TRUE(port.tx_begin(64).empty());
    auto const committed = port.tx_commit({}, ieee::Eui48{}, 2, 3);
    EXPECT_TRUE(!committed && committed.error() == std::errc::not_supported);
    EXPECT_TRUE(port.tx_flush().has_value());
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

#endif  // __linux__ && HAVE_XDP

TEST_MAIN(statusbar_avb_entity, avb_entity_xdp_stream_port_test)
