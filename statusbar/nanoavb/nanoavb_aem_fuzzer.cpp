// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for AemCommandHandler::process_packet (IEEE 1722.1-2021 Clause 7.4, Milan v1.3
/// Section 5.4.4) and parse_announce_path_trace: AEM and MVU commands from up to twenty controllers,
/// every frame sent and the state after each command keeping the invariants the checks below name.

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/atdecc/atdecc_utf8.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/gptp/gptp_messages.hpp"
#include "statusbar/gptp/gptp_tlv.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/sm/sm.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

ieee::Eui64 const US{0xAA, 0, 0, 0, 0, 0, 0, 0x71};

/// More controllers than MAX_UNSOLICITED_REGISTRATIONS, so a registration can be refused.
constexpr size_t CONTROLLERS = 20;
static_assert(CONTROLLERS > AemCommandHandler::MAX_UNSOLICITED_REGISTRATIONS);

/// A command record: a length octet, a kind octet, a sender octet, then that much data.
constexpr size_t RECORD_HEADER = 3;
/// Kind octet: with PICK set, its low bits choose from COMMANDS; with ANSWER (and not PICK), the
/// record is the sender's AEM response to the entity's CONTROLLER_AVAILABLE; otherwise the low bits
/// are an AEM command type.
constexpr uint8_t KIND_PICK = 0x80U;
constexpr uint8_t KIND_ANSWER = 0x40U;
constexpr uint8_t KIND_TYPE_MASK = 0x3FU;
/// Sender octet: the controller in the low bits, another MAC with MAC_MOVED, the time that passes
/// after the command in the top two bits (STEPS).
constexpr uint8_t SENDER_MASK = 0x1FU;
constexpr uint8_t SENDER_MAC_MOVED = 0x20U;
constexpr unsigned SENDER_STEP_SHIFT = 6;
/// 10 ms, 1 s, past the controller monitor's least period, past its greatest.
constexpr std::array<std::chrono::milliseconds, 4> STEPS{
    std::chrono::milliseconds{10}, std::chrono::seconds{1}, std::chrono::seconds{31}, std::chrono::seconds{61}};

/// First input octet: bit 7 has every hook refuse with the low bits, bit 6 selects milan13, bit 5
/// makes the entity a Milan device (MVU answered).
constexpr uint8_t CONTROL_REFUSES = 0x80U;
constexpr uint8_t CONTROL_MILAN13 = 0x40U;
constexpr uint8_t CONTROL_MILAN_INFO = 0x20U;
constexpr uint8_t CONTROL_VERDICT_MASK = 0x1FU;

/// A command COMMANDS can name: AEM, MVU, or a change of the AS path told to the handler.
enum class Kind : uint8_t
{
    aem,
    mvu,
    path_changed,
};

/// One entry of COMMANDS.
struct Command
{
    Kind kind{Kind::aem};
    uint16_t type{0};
};

/// AEM command @p type.
constexpr auto aem(uint16_t const type) -> Command
{
    return Command{.kind = Kind::aem, .type = type};
}
/// MVU command @p type (Milan v1.3 Table 5.15).
constexpr auto mvu(uint16_t const type) -> Command
{
    return Command{.kind = Kind::mvu, .type = type};
}

/// The commands this handler serves, every one the AEM additions bring among them.
constexpr std::array<Command, 36> COMMANDS{
    aem(AEM_COMMAND_GET_AS_PATH),
    aem(AEM_COMMAND_SET_MAX_TRANSIT_TIME),
    aem(AEM_COMMAND_GET_MAX_TRANSIT_TIME),
    aem(AEM_COMMAND_GET_AUDIO_MAP),
    aem(AEM_COMMAND_ADD_AUDIO_MAPPINGS),
    aem(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS),
    aem(AEM_COMMAND_SET_SAMPLING_RATE),
    aem(AEM_COMMAND_GET_SAMPLING_RATE),
    aem(AEM_COMMAND_SET_CONFIGURATION),
    aem(AEM_COMMAND_GET_CONFIGURATION),
    aem(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION),
    aem(AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION),
    aem(AEM_COMMAND_CONTROLLER_AVAILABLE),
    aem(AEM_COMMAND_LOCK_ENTITY),
    aem(AEM_COMMAND_ACQUIRE_ENTITY),
    aem(AEM_COMMAND_SET_STREAM_FORMAT),
    aem(AEM_COMMAND_GET_STREAM_FORMAT),
    aem(AEM_COMMAND_SET_NAME),
    aem(AEM_COMMAND_GET_NAME),
    aem(AEM_COMMAND_SET_CONTROL),
    aem(AEM_COMMAND_GET_STREAM_INFO),
    aem(AEM_COMMAND_GET_COUNTERS),
    aem(AEM_COMMAND_GET_AVB_INFO),
    aem(AEM_COMMAND_READ_DESCRIPTOR),
    aem(AEM_COMMAND_START_STREAMING),
    aem(AEM_COMMAND_STOP_STREAMING),
    aem(AEM_COMMAND_SET_CLOCK_SOURCE),
    aem(AEM_COMMAND_GET_DYNAMIC_INFO),
    mvu(MVU_COMMAND_GET_MILAN_INFO),
    mvu(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID),
    mvu(MVU_COMMAND_GET_SYSTEM_UNIQUE_ID),
    mvu(MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO),
    mvu(MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO),
    mvu(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX),
    mvu(MVU_COMMAND_BIND_STREAM),
    Command{.kind = Kind::path_changed},
};

/// avb_entity's rate model (dynamic stream ports, one CLOCK_DOMAIN), kept with the kit tests that
/// use it too; a tree without the file parses a one-octet model, which model_storage() refuses.
// NOLINTNEXTLINE(*-avoid-c-arrays): #embed fills a braced array
constexpr uint8_t MODEL[] = {
#if __has_embed("../avb_entity/testdata/entity_rates.bin")
#    embed "../avb_entity/testdata/entity_rates.bin"
#else
    0
#endif
};

/// The test model, parsed once; a malformed one aborts the run.
auto model_storage() -> DescriptorStorage const&
{
    static auto const storage = DescriptorStorage::create(std::span<uint8_t const>{MODEL});
    if (!storage) {
        std::fputs("nanoavb_aem_fuzzer: entity_rates.bin missing or malformed\n", stderr);
        std::abort();
    }
    return *storage;
}

/// Stop the run when an invariant does not hold.
void require(bool const holds)
{
    if (!holds) {
        __builtin_trap();
    }
}

/// One frame the handler sent.
struct Sent
{
    std::vector<uint8_t> frame;

    [[nodiscard]] auto common() const -> AecpDuCommon
    {
        AecpDuCommon c{};
        span_load(c, std::span<uint8_t const>{frame});
        return c;
    }
    [[nodiscard]] auto is_mvu() const -> bool { return common().is_vendor_unique(); }
    [[nodiscard]] auto header_length() const -> size_t { return is_mvu() ? AecpMvuDu::LENGTH : AemDu::LENGTH; }
    [[nodiscard]] auto body() const -> std::span<uint8_t const> { return std::span<uint8_t const>{frame}.subspan(header_length()); }
    [[nodiscard]] auto type() const -> uint16_t
    {
        if (is_mvu()) {
            AecpMvuDu du{};
            span_load(du, std::span<uint8_t const>{frame});
            return du.command_type();
        }
        AemDu du{};
        span_load(du, std::span<uint8_t const>{frame});
        return du.command_code();
    }
    [[nodiscard]] auto unsolicited() const -> bool
    {
        if (is_mvu()) {
            AecpMvuDu du{};
            span_load(du, std::span<uint8_t const>{frame});
            return du.is_unsolicited();
        }
        AemDu du{};
        span_load(du, std::span<uint8_t const>{frame});
        return du.is_unsolicited();
    }
    [[nodiscard]] auto is_response() const -> bool
    {
        auto const type = common().message_type();
        return type == AECP_MESSAGE_TYPE_AEM_RESPONSE || type == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE;
    }
};

/// The entity under test and what it has sent.
struct Rig
{
    DescriptorStorageHandler model;
    AemCommandHandler aem;
    DynamicAudioMaps& maps;
    std::vector<Sent> sent{};
    AsPath path{};
    bool milan13{false};
    uint16_t sequence_id{0};
    sm::TimePoint now{std::chrono::seconds{1}};

    explicit Rig(uint8_t const control, DynamicAudioMaps& store)
        : model{model_storage()}
        , aem{model}
        , maps{store}
        , milan13{(control & CONTROL_MILAN13) != 0}
    {
        maps.clear();
        aem.set_dynamic_audio_maps(&maps);
        aem.set_entity_id(US);
        aem.set_profile(milan13 ? AemCommandHandler::Profile::milan13 : AemCommandHandler::Profile::classic);
        if ((control & CONTROL_MILAN_INFO) != 0) {
            aem.set_milan_info(AemCommandHandler::MilanInfo{});
        }
        aem.set_send_response([this](ieee::Eui48 const&, std::span<uint8_t const> const frame) {
            sent.push_back(Sent{.frame = {frame.begin(), frame.end()}});
            return true;
        });
    }
};

/// Every hook answers @p verdict; the AS path is the Announce's, the offsets a talker's.
void install_hooks(Rig& rig, uint8_t const verdict)
{
    rig.model.manage_object_names();
    rig.model.set_on_stream_format_changed([verdict](uint16_t, uint16_t, uint64_t) { return verdict; });
    rig.model.set_on_sampling_rate_changed([verdict](uint16_t, uint32_t) { return verdict; });
    rig.model.set_on_entity_name_changed([verdict](AtdeccString const&) { return verdict; });
    rig.model.set_on_streaming_changed([verdict](uint16_t, uint16_t, bool) { return verdict; });
    rig.aem.set_on_configuration_changed([verdict](uint16_t) { return verdict; });
    rig.aem.set_on_audio_mappings_changed([verdict](uint16_t, uint16_t, std::span<DynamicAudioMapping const>) { return verdict; });
    rig.aem.set_get_as_path([&rig](uint16_t const index, AsPath& path) {
        path = rig.path;
        return index == 0;
    });
    rig.aem.set_get_stream_info([](uint16_t, uint16_t const index, AemStreamInfoPayload&) { return index < 2; });
    rig.aem.set_presentation_offset_hooks(
        [](uint16_t const index) { return index < 2 ? std::optional<uint32_t>{2'000'000} : std::nullopt; },
        [verdict](uint16_t, uint32_t) { return verdict; });
}

/// The controller and MAC a sender octet names.
auto controller_of(uint8_t const sender) -> ieee::Eui64
{
    return ieee::Eui64{0xC0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>((sender & SENDER_MASK) % CONTROLLERS)};
}
auto mac_of(uint8_t const sender) -> ieee::Eui48
{
    auto const moved = static_cast<uint8_t>((sender & SENDER_MAC_MOVED) != 0 ? 0x80U : 0U);
    return ieee::Eui48{0x02, 0, 0, 0, moved, static_cast<uint8_t>((sender & SENDER_MASK) % CONTROLLERS)};
}

/// The frame of an AEM command from the sender.
auto aem_frame(Rig const& rig, uint16_t const type, uint8_t const sender, std::span<uint8_t const> const data)
    -> std::vector<uint8_t>
{
    AemDu du{};
    du.init_command(type, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + data.size()));
    du.target_entity_id = US;
    du.controller_entity_id = controller_of(sender);
    du.sequence_id = rig.sequence_id;
    std::vector<uint8_t> frame(AemDu::LENGTH + data.size());
    span_store(std::span{frame}, du);
    std::ranges::copy(data, frame.begin() + AemDu::LENGTH);
    return frame;
}

/// The sender's answer to the entity's CONTROLLER_AVAILABLE, with the sequence id @p data begins with
/// (IEEE 1722.1-2021 Clause 7.4.4): the entity is its controller.
auto answer_frame(uint8_t const sender, std::span<uint8_t const> const data) -> std::vector<uint8_t>
{
    ieee::doublet_t sequence_id{0};
    if (data.size() >= sizeof(sequence_id)) {
        span_load(sequence_id, data);
    }
    AemDu du{};
    du.init_response(AEM_COMMAND_CONTROLLER_AVAILABLE, AEM_STATUS_SUCCESS, AemDu::AEM_DATA_LENGTH);
    du.target_entity_id = controller_of(sender);
    du.controller_entity_id = US;
    du.sequence_id = sequence_id;
    std::vector<uint8_t> frame(AemDu::LENGTH);
    span_store(std::span{frame}, du);
    return frame;
}

auto mvu_frame(Rig const& rig, uint16_t const type, uint8_t const sender, std::span<uint8_t const> const data)
    -> std::vector<uint8_t>
{
    AecpMvuDu du{};
    du.init_command(type, static_cast<uint16_t>(data.size()));
    du.common.target_entity_id = US;
    du.common.controller_entity_id = controller_of(sender);
    du.common.sequence_id = rig.sequence_id;
    std::vector<uint8_t> frame(AecpMvuDu::LENGTH + data.size());
    span_store(std::span{frame}, du);
    std::ranges::copy(data, frame.begin() + AecpMvuDu::LENGTH);
    return frame;
}

/// A path trace read from @p announce lies inside it, behind the Announce and its TLV header,
/// and within MAX_AS_PATH_LENGTH.
void check_path(AsPath const& path, std::span<uint8_t const> const announce)
{
    size_t const read = gptp::AnnounceMessage::LENGTH + gptp::TlvHeader::LENGTH + (path.size() * ieee::Eui64::LENGTH);
    require(path.size() <= MAX_AS_PATH_LENGTH && (path.empty() || read <= announce.size()));
}

/// Every frame: control_data_length agrees with its length, and its body fits a response, an
/// IEEE 1722.1 AECPDU under classic (Clause 9.2.1.1.7).
void check_frame(Rig const& rig, Sent const& s)
{
    require(s.frame.size() >= AecpDuCommon::HEADER_LENGTH + AecpDuCommon::COMMON_DATA_LENGTH);
    require(s.frame.size() == AecpDuCommon::HEADER_LENGTH + s.common().control_data_length());
    require(s.frame.size() >= s.header_length());
    require(s.body().size() <= MAX_AEM_RESPONSE_SIZE);
    require(rig.milan13 || s.body().size() <= MAX_AEM_BODY);
}

/// A MEDIA_CLOCK_REFERENCE_INFO response carries the settings (74 octets), or the GET given back
/// for a domain the model lacks, or nothing for a command too short.
void check_mcr(Sent const& s)
{
    AecpMvuDu du{};
    span_load(du, std::span<uint8_t const>{s.frame});
    auto const status = du.common.status();
    bool const get = du.command_type() == MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO;
    size_t const expected = status == MVU_STATUS_PAYLOAD_TOO_SHORT ? 0
        : get && status == MVU_STATUS_NO_SUCH_DESCRIPTOR           ? MvuGetMediaClockReferenceInfoCommand::LENGTH
                                                                   : MvuMediaClockReferenceInfoPayload::LENGTH;
    require(s.body().size() == expected);
}

/// A SYSTEM_UNIQUE_ID response carries the id in force, system_unique_id() octet for octet (74, the
/// reserved ones zero), or nothing for a command too short.
void check_system_unique_id(AemCommandHandler const& aem, Sent const& s)
{
    AecpMvuDu du{};
    span_load(du, std::span<uint8_t const>{s.frame});
    std::array<uint8_t, MvuSystemUniqueIdPayload::LENGTH> in_force{};
    span_store(std::span{in_force}, aem.system_unique_id());
    bool const short_command = du.common.status() == MVU_STATUS_PAYLOAD_TOO_SHORT;
    require(short_command ? s.body().empty() : (std::ranges::equal(s.body(), in_force) && in_force[0] == 0 && in_force[1] == 0));
}

/// An MVU frame of the commands whose responses the checks above know.
void check_mvu(Rig const& rig, Sent const& s)
{
    if (s.type() == MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO || s.type() == MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO) {
        check_mcr(s);
    }
    if (s.type() == MVU_COMMAND_GET_SYSTEM_UNIQUE_ID || s.type() == MVU_COMMAND_SET_SYSTEM_UNIQUE_ID) {
        check_system_unique_id(rig.aem, s);
    }
}

/// A GET_AUDIO_MAP answered SUCCESS counts what it carries, no more than a partition.
void check_audio_map(Rig const& rig, Sent const& s)
{
    AemDu du{};
    span_load(du, std::span<uint8_t const>{s.frame});
    if (du.command_code() != AEM_COMMAND_GET_AUDIO_MAP || du.status() != AEM_STATUS_SUCCESS) {
        return;
    }
    require(s.body().size() >= AemAudioMapResponseHeader::LENGTH);
    AemAudioMapResponseHeader header{};
    span_load(header, s.body());
    size_t const count = header.number_of_mappings.get();
    size_t const partition = rig.milan13 ? AemCommandHandler::MILAN_AUDIO_MAP_MAPPINGS : AemCommandHandler::AUDIO_MAP_MAPPINGS;
    require(s.body().size() == AemAudioMapResponseHeader::LENGTH + (count * AemAudioMapping::LENGTH) && count <= partition);
}

/// A GET_AS_PATH answered SUCCESS, or notified, counts the identities it carries, within a path.
void check_as_path(Sent const& s)
{
    AemDu du{};
    span_load(du, std::span<uint8_t const>{s.frame});
    if (du.command_code() != AEM_COMMAND_GET_AS_PATH || du.status() != AEM_STATUS_SUCCESS) {
        return;
    }
    require(s.body().size() >= AemAsPathResponseHeader::LENGTH);
    AemAsPathResponseHeader header{};
    span_load(header, s.body());
    size_t const count = header.count.get();
    require(count <= MAX_AS_PATH_LENGTH && s.body().size() == AemAsPathResponseHeader::LENGTH + (count * ieee::Eui64::LENGTH));
}

/// A GET_STREAM_INFO answered SUCCESS, or notified, is IEEE 1722.1-2021's 84 octets under milan13
/// (Milan v1.3 Section 5.4.2.10), its IP tail zero, 1722.1-2013's 48 under classic.
void check_stream_info(Rig const& rig, Sent const& s)
{
    AemDu du{};
    span_load(du, std::span<uint8_t const>{s.frame});
    if (du.command_code() != AEM_COMMAND_GET_STREAM_INFO || du.status() != AEM_STATUS_SUCCESS) {
        return;
    }
    require(s.body().size() == (rig.milan13 ? AEM_STREAM_INFO_2021_LENGTH : AemStreamInfoPayload::LENGTH));
    require(std::ranges::all_of(s.body().subspan(AemStreamInfoPayload::LENGTH), [](uint8_t const b) { return b == 0; }));
}

/// The frames sent from @p first for a command of @p kind and @p type with @p sequence_id: each
/// well formed; the solicited response echoes the command; no notification of it comes first.
void check_sent(Rig const& rig, size_t const first, Kind const kind, uint16_t const type, uint16_t const sequence_id)
{
    bool answered = false;
    for (auto const& s : std::span{rig.sent}.subspan(first)) {
        check_frame(rig, s);
        bool const same = s.is_mvu() == (kind == Kind::mvu) && s.type() == type;
        if (s.is_mvu()) {
            check_mvu(rig, s);
        }
        if (!s.is_mvu() && s.is_response()) {
            check_audio_map(rig, s);
            check_as_path(s);
            check_stream_info(rig, s);
        }
        if (s.is_response() && !s.unsolicited() && same) {
            require(s.common().sequence_id.get() == sequence_id);
            answered = true;
        }
        require(answered || !(same && s.unsolicited()));  // Milan v1.3 Section 5.4.5.3
    }
}

/// The handler's state: the mapping store sorted, unique, within capacity, and every mapping in it
/// still holding against the model (Milan v1.3 Sections 5.4.2.7, 5.4.2.13); the system name in force
/// well-formed UTF-8 (Section 5.4.4.2); the registrations and the deferred commands within their capacities.
void check_state(AemCommandHandler const& aem, DynamicAudioMaps const& maps)
{
    require(maps.size() <= DynamicAudioMaps::MAX_MAPPINGS && aem.mappings_hold());
    std::optional<std::pair<uint32_t, DynamicAudioMapping>> previous{};
    maps.for_each([&previous](uint16_t const type, uint16_t const index, DynamicAudioMapping const& m) {
        uint32_t const port = (uint32_t{type} << 16U) | index;
        require(!previous || previous->first < port || (previous->first == port && cluster_order(previous->second, m)));
        previous = std::pair{port, m};
    });
    require(atdecc::utf8_well_formed(aem.system_unique_id().system_name.as_string_view()));
    require(aem.unsolicited_registration_count() <= AemCommandHandler::MAX_UNSOLICITED_REGISTRATIONS);
    require(aem.pending_command_count() <= AemCommandHandler::MAX_PENDING_COMMANDS);
}

/// One record: the command (or answer, or path change) it names, its frames checked, then the clock.
void run_record(Rig& rig, uint8_t const kind_octet, uint8_t const sender, std::span<uint8_t const> const data)
{
    bool const picked = (kind_octet & KIND_PICK) != 0;
    bool const answer = !picked && (kind_octet & KIND_ANSWER) != 0;
    Command const command = picked ? COMMANDS[(kind_octet & KIND_TYPE_MASK) % COMMANDS.size()]
                                   : aem(static_cast<uint16_t>(kind_octet & KIND_TYPE_MASK));
    size_t const first = rig.sent.size();
    ++rig.sequence_id;
    rig.aem.set_event_time(rig.now);  // as the host's net handler does for every packet
    if (command.kind == Kind::path_changed) {
        rig.path = parse_announce_path_trace(data);
        check_path(rig.path, data);
        (void)rig.aem.notify_as_path_changed(0);
    } else {
        auto const frame = answer       ? answer_frame(sender, data)
            : command.kind == Kind::mvu ? mvu_frame(rig, command.type, sender, data)
                                        : aem_frame(rig, command.type, sender, data);
        (void)rig.aem.process_packet(mac_of(sender), frame, US);
    }
    check_sent(rig, first, answer ? Kind::path_changed : command.kind, command.type, rig.sequence_id);
    check_state(rig.aem, rig.maps);
    rig.now += STEPS[sender >> SENDER_STEP_SHIFT];
    size_t const ticked = rig.sent.size();
    rig.aem.tick(rig.now);
    for (auto const& s : std::span{rig.sent}.subspan(ticked)) {
        check_frame(rig, s);
    }
    check_state(rig.aem, rig.maps);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> input{data, size};
    if (input.empty()) {
        return 0;
    }
    uint8_t const control = input[0];
    uint8_t const verdict = (control & CONTROL_REFUSES) == 0 ? AEM_STATUS_SUCCESS : (control & CONTROL_VERDICT_MASK);
    static DynamicAudioMaps maps;  // the entity's store: 20 KB, kept off the stack
    Rig rig{control, maps};
    install_hooks(rig, verdict);
    input = input.subspan(1);

    // The input whole: an Announce whose path trace is the AS path, and one AECPDU.
    rig.path = parse_announce_path_trace(input);
    check_path(rig.path, input);
    (void)rig.aem.process_packet(mac_of(0), input, US);
    for (auto const& s : rig.sent) {
        check_frame(rig, s);
    }
    check_state(rig.aem, rig.maps);

    while (input.size() >= RECORD_HEADER) {
        size_t const length = std::min<size_t>(input[0], input.size() - RECORD_HEADER);
        run_record(rig, input[1], input[2], input.subspan(RECORD_HEADER, length));
        input = input.subspan(RECORD_HEADER + length);
    }
    return 0;
}
