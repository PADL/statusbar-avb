// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ListenerVlan over a recording VlanControl: create, move, release, EEXIST and EPERM handling,
// and the kit's wiring (configured VID, adopted MSRP Domain VID, back on link up, stop()).

#include "statusbar/test/test.hpp"

#if defined(__linux__)

#    include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#    include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#    include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#    include "statusbar/avb_entity/avb_entity_identity.hpp"
#    include "statusbar/avb_entity/avb_entity_kit.hpp"
#    include "statusbar/avb_entity/avb_entity_listener_vlan.hpp"
#    include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"

#    include <atomic>
#    include <chrono>
#    include <cstdint>
#    include <format>
#    include <memory_resource>
#    include <optional>
#    include <print>
#    include <span>
#    include <string>
#    include <string_view>
#    include <system_error>
#    include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

/// Records each request as `ip` would spell it; answers with @c next (then success).
class RecordingVlan final : public net::vlan::VlanControl
{
  public:
    std::vector<std::string> requests;
    std::optional<std::errc> next{};

    auto add_vlan(int const parent_ifindex, std::string_view const name, uint16_t const vid) -> Status override
    {
        requests.push_back(std::format("add {} {} {}", parent_ifindex, name, vid));
        return answer();
    }
    auto remove_link(std::string_view const name) -> Status override
    {
        requests.push_back(std::format("del {}", name));
        return answer();
    }
    [[nodiscard]] auto last_message() const noexcept -> std::string const& override { return message_; }

  private:
    auto answer() -> Status
    {
        if (next) {
            auto const e = *next;
            next.reset();
            return failure(std::make_error_code(e));
        }
        return success();
    }
    std::string message_{};
};

auto same(std::vector<std::string> const& got, std::vector<std::string> const& want) -> bool
{
    if (got != want) {
        for (auto const& r : got) {
            std::println("  got: {}", r);
        }
        return false;
    }
    return true;
}

}  // namespace

TEST(listener_vlan, creates_moves_and_deletes_its_own_link)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingVlan fake;
    {
        ListenerVlan vlan{channel.logger()};
        vlan.follow(2);  // unbound: nothing
        EXPECT_TRUE(fake.requests.empty());
        vlan.bind(fake, "enp8s0", 3);
        vlan.follow(2);
        EXPECT_TRUE(vlan.created());
        EXPECT_EQ(vlan.vid(), uint16_t{2});
        EXPECT_TRUE(vlan.link_name() == "enp8s0.2");
        vlan.follow(2);  // the same VID: nothing
        vlan.follow(7);  // the new one first, then ours goes
        vlan.release();
        EXPECT_EQ(vlan.vid(), uint16_t{0});
        EXPECT_FALSE(vlan.created());
        vlan.follow(2);  // released: unbound, nothing
        EXPECT_FALSE(vlan.bound());
        vlan.bind(fake, "enp8s0", 3);
        vlan.follow(2);  // the destructor deletes it
    }
    EXPECT_TRUE(same(
        fake.requests,
        {"add 3 enp8s0.2 2", "add 3 enp8s0.7 7", "del enp8s0.2", "del enp8s0.7", "add 3 enp8s0.2 2", "del enp8s0.2"}));
}

TEST(listener_vlan, leaves_an_existing_link_and_survives_a_refusal)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingVlan fake;
    ListenerVlan vlan{channel.logger()};
    vlan.bind(fake, "enp8s0", 3);
    fake.next = std::errc::file_exists;  // an operator's enp8s0.2
    vlan.follow(2);
    EXPECT_FALSE(vlan.created());
    EXPECT_EQ(vlan.vid(), uint16_t{2});
    vlan.follow(0);     // not a VID: nothing
    vlan.follow(4095);  // reserved: nothing
    EXPECT_EQ(vlan.vid(), uint16_t{2});
    fake.next = std::errc::operation_not_permitted;  // no CAP_NET_ADMIN
    vlan.follow(7);                                  // changes nothing
    EXPECT_EQ(vlan.vid(), uint16_t{2});
    vlan.follow(7);  // tried again; the existing enp8s0.2 is not deleted
    EXPECT_TRUE(vlan.created());
    EXPECT_EQ(vlan.vid(), uint16_t{7});
    vlan.release();
    EXPECT_TRUE(same(fake.requests, {"add 3 enp8s0.2 2", "add 3 enp8s0.7 7", "add 3 enp8s0.7 7", "del enp8s0.7"}));
}

namespace {

/// AAF 48 kHz INT_24, 2 channels, 6 frames per packet (class A).
constexpr uint64_t AAF_48K_INT24_2CH = 0x0205031800000000ULL | (uint64_t{2} << 22) | (uint64_t{6} << 12);

template <typename Descriptor>
auto wire(Descriptor const& desc) -> std::span<uint8_t const>
{
    return std::span{reinterpret_cast<uint8_t const*>(&desc), desc.wire_size()};
}

/// ENTITY + CONFIGURATION + AVB_INTERFACE, one AAF STREAM_OUTPUT and @p inputs AAF STREAM_INPUTs.
auto make_blob(uint16_t const inputs) -> std::vector<uint8_t>
{
    using namespace atdecc::aem;
    nanoavb::AemBlobWriter w;
    DescriptorEntity entity{};
    entity.configurations_count = 1;
    entity.talker_stream_sources = 1;
    entity.listener_stream_sinks = inputs;
    w.add(0, DESCRIPTOR_ENTITY, 0, make_const_span(entity));
    DescriptorConfiguration config{};
    w.add(0, DESCRIPTOR_CONFIGURATION, 0, make_const_span(config));
    DescriptorStream output{};
    output.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    output.current_format.from_uint64(AAF_48K_INT24_2CH);
    w.add(0, DESCRIPTOR_STREAM_OUTPUT, 0, wire(output));
    for (uint16_t index = 0; index < inputs; ++index) {
        DescriptorStream input{};
        input.descriptor_type = DESCRIPTOR_STREAM_INPUT;
        input.descriptor_index = index;
        input.current_format.from_uint64(AAF_48K_INT24_2CH);
        w.add(0, DESCRIPTOR_STREAM_INPUT, index, wire(input));
    }
    DescriptorAvbInterface iface{};
    w.add(0, DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(iface));
    return w.write();
}

/// Run @p body with a kit over make_blob(@p inputs) on interface "vlantest0".
template <typename Body>
void with_kit(uint16_t const inputs, Body const& body)
{
    auto const blob = make_blob(inputs);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob});
    EXPECT_TRUE(storage.has_value());
    auto const talkers = *talker_stream_specs(*storage, 0);
    auto const listeners = *listener_stream_specs(*storage, 0);
    AvbEntityAudioIOConfig config{};
    config.interface_name = "vlantest0";  // no such interface: no MAC, no sockets
    config.entity_id = ieee::Eui64{0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x04};
    config.stream_address_mode = "static";
    EntityIdentityDescriptorHandler handler{
        *storage, config.entity_id, ieee::Eui64{}, "1.0.0", "kit vlan", std::nullopt, /*patch_avb_interface=*/false};
    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<uint64_t> last_gptp_ns{0};
    AvbEntityKit kit{
        config,
        handler,
        talkers,
        listeners,
        48000,
        /*initial_clock_source=*/0,
        /*crf_clock_source_index=*/std::nullopt,
        media_clock,
        last_gptp_ns,
        /*audio_sink=*/nullptr,
        std::pmr::get_default_resource()};
    body(kit);
}

}  // namespace

TEST(listener_vlan, the_kit_follows_the_class_a_vid)
{
    RecordingVlan fake;
    with_kit(1, [&fake](AvbEntityKit& kit) {
        kit.bind_listener_vlan(fake, 9);
        EXPECT_TRUE(kit.listener_vlan().created());

        // A bridge's Class A Domain on VID 7 is adopted, then a link up returns to 2.
        auto& components = kit.host().components();
        std::vector<std::vector<uint8_t>> bridge_out;
        srp::msrp::MsrpParticipant bridge{srp::msrp::MsrpConfig{}, 0xb1d9e};
        bridge.set_send_pdu([&bridge_out](std::span<uint8_t const> p) {
            bridge_out.emplace_back(p.begin(), p.end());
            return true;
        });
        auto now = sm::Clock::now();
        bridge.start(now);
        srp::msrp::DomainFirstValue class_a{};
        class_a.sr_class_vid = 7;
        (void)bridge.declare_domain(class_a, now);
        for (int i = 0; i < 4; ++i) {
            now += std::chrono::milliseconds(250);
            bridge.tick(now);
            for (auto const& p : bridge_out) {
                components.msrp_handler.receive_packet(p, now);
            }
            bridge_out.clear();
        }
        EXPECT_EQ(kit.listener_vlan().vid(), uint16_t{7});
        kit.on_link_up(now);
        EXPECT_EQ(kit.listener_vlan().vid(), uint16_t{2});
        (void)kit.stop();  // not running, but the link goes and the kit unbinds
        EXPECT_EQ(kit.listener_vlan().vid(), uint16_t{0});
        EXPECT_FALSE(kit.listener_vlan().bound());
    });
    EXPECT_TRUE(same(
        fake.requests,
        {"add 9 vlantest0.2 2",
         "add 9 vlantest0.7 7",
         "del vlantest0.2",
         "add 9 vlantest0.2 2",
         "del vlantest0.7",
         "del vlantest0.2"}));
}

TEST(listener_vlan, a_talker_only_kit_registers_nothing)
{
    RecordingVlan fake;
    with_kit(0, [&fake](AvbEntityKit& kit) {
        kit.bind_listener_vlan(fake, 9);
        EXPECT_FALSE(kit.listener_vlan().bound());
    });
    EXPECT_TRUE(fake.requests.empty());
}

#endif  // __linux__

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_vlan_test)
