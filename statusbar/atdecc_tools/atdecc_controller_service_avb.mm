// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The macOS ControllerService backend over the system AVB framework
/// (AudioVideoBridging). The framework owns the 1722.1 protocol on this
/// platform — discovery state machines, AECP/ACMP inflight and retry —
/// and it is the only path that reaches an entity hosted by the same
/// machine (the macOS virtual entity), whose traffic never appears on
/// the wire.
///
/// Threading: the framework delivers delegate callbacks and command
/// completions on its own dispatch queues. Every event is captured into
/// plain C++ values on the framework thread and posted through a
/// ReactorMarshal; sink callbacks and per-command completions therefore
/// run on the reactor thread, exactly like the raw-socket backend.
///
/// Scope: controller-only. This backend never publishes a local entity;
/// our entities live on Linux / bare metal. Fields the framework does
/// not expose per-entity (valid_time cadence, some gPTP detail) are
/// approximations — nothing above the seam may depend on them.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_service.hpp"
#include "statusbar/atdecc_tools/atdecc_reactor_marshal.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>

#import <AudioVideoBridging/AudioVideoBridging.h>
#import <Foundation/Foundation.h>

namespace statusbar::atdecc_tools {
class MacAvbControllerService;
}

/// Objective-C adapter: receives framework delegate callbacks on framework
/// threads and forwards them to the reactor. Lifetime model: the bridge
/// NEVER dereferences `owner` on a framework thread — it only captures the
/// pointer value and posts a task through the shared marshal; the task
/// checks `alive` (flipped by the service destructor on the reactor thread,
/// where tasks also run) before touching the service. `owner` is atomic so
/// the destructor's clear cannot race the framework-thread reads, and the
/// shared_ptr ivars keep the marshal and flag alive for callbacks that fire
/// after the service is gone.
@interface StatusbarAvbBridge : NSObject<AVB17221EntityDiscoveryDelegate, AVB17221AECPClient>
{
  @public
    std::atomic<statusbar::atdecc_tools::MacAvbControllerService*> owner;
    std::shared_ptr<std::atomic<bool>> alive;
    std::shared_ptr<statusbar::atdecc_tools::ReactorMarshal> marshal;
}
@end

namespace statusbar::atdecc_tools {

using namespace statusbar::atdecc;
using ieee::Eui48;
using ieee::Eui64;

/// Everything we lift out of an AVB17221Entity, captured as plain values
/// on the framework thread before marshaling.
struct EntitySnapshot
{
    uint64_t entity_id{0};
    uint64_t entity_model_id{0};
    uint32_t entity_capabilities{0};
    uint16_t talker_stream_sources{0};
    uint16_t talker_capabilities{0};
    uint16_t listener_stream_sinks{0};
    uint16_t listener_capabilities{0};
    uint32_t controller_capabilities{0};
    uint32_t available_index{0};
    uint64_t gptp_grandmaster_id{0};
    uint8_t gptp_domain_number{0};
    uint16_t identify_control_index{0};
    uint16_t interface_index{0};
    uint64_t association_id{0};
    Eui48 mac{};
    bool local{false};
};

auto snapshot_of(AVB17221Entity* entity) -> EntitySnapshot
{
    EntitySnapshot s{};
    s.entity_id = entity.entityID;
    s.entity_model_id = entity.entityModelID;
    s.entity_capabilities = static_cast<uint32_t>(entity.entityCapabilities);
    s.talker_stream_sources = entity.talkerStreamSources;
    s.talker_capabilities = static_cast<uint16_t>(entity.talkerCapabilities);
    s.listener_stream_sinks = entity.listenerStreamSinks;
    s.listener_capabilities = static_cast<uint16_t>(entity.listenerCapabilities);
    s.controller_capabilities = static_cast<uint32_t>(entity.controllerCapabilities);
    s.available_index = entity.availableIndex;
    s.gptp_grandmaster_id = entity.gPTPGrandmasterID;
    s.gptp_domain_number = entity.gPTPDomainNumber;
    s.identify_control_index = entity.identifyControlIndex;
    s.interface_index = entity.interfaceIndex;
    s.association_id = entity.associationID;
    s.local = entity.isLocalEntity;
    if (entity.macAddresses.count > 0) {
        (void)ieee::load_unchecked(std::span<uint8_t const>{entity.macAddresses[0].bytes, Eui48::LENGTH}, &s.mac);
    }
    return s;
}

auto eui64_of(uint64_t const v) -> Eui64
{
    return Eui64{}.from_uint64(v);
}

/// Store one big-endian doublet into a payload buffer via the ieee
/// network-order helpers (no hand-rolled shift/mask packing).
inline void put_u16(std::span<uint8_t> const out, size_t const offset, uint16_t const v)
{
    (void)ieee::store(out.subspan(offset), doublet_t{v});
}

/// A marshaled AEM outcome: owned copies of the payloads plus the
/// caller's completion. Sized for ReactorMarshal::TASK_CAPACITY.
struct AemOutcome
{
    AemCommandDelivery delivery{AemCommandDelivery::TimedOut};
    uint8_t status{0};
    uint16_t command_type{0};
    Eui64 target{};
    statusbar::sg14::inplace_vector<uint8_t, 64> sent;  // request payloads are small (<= 16B today)
    statusbar::sg14::inplace_vector<uint8_t, 512> response;
};

class MacAvbControllerService final : public ControllerService
{
  public:
    MacAvbControllerService(std::string const& interface_name, Eui64 controller_id)
        : controller_id_{controller_id}
    {
        if (!marshal_->valid()) {
            return;
        }
        NSString* name = [NSString stringWithUTF8String:interface_name.c_str()];
        interface_ = [[AVBEthernetInterface alloc] initWithInterfaceName:name];
        if (interface_ == nil || interface_.entityDiscovery == nil || interface_.aecp == nil || interface_.acmp == nil) {
            interface_ = nil;
            return;
        }
        bridge_ = [[StatusbarAvbBridge alloc] init];
        bridge_->owner.store(this);
        bridge_->alive = alive_;
        bridge_->marshal = marshal_;
        interface_.entityDiscovery.discoveryDelegate = bridge_;
        valid_ = true;
    }

    ~MacAvbControllerService() override
    {
        // Destruction happens on the reactor thread — the same thread that
        // runs marshaled tasks — so flipping `alive_` here guarantees any
        // task that observes alive==true also observes a live service.
        // Framework completion blocks still in flight hold shared_ptr copies
        // of the marshal and flag, so their late posts land in a marshal
        // that outlives us and their tasks no-op on the dead flag. Their
        // per-command completions are dropped at teardown (documented in
        // the ControllerService contract).
        alive_->store(false);
        if (interface_ != nil) {
            interface_.entityDiscovery.discoveryDelegate = nil;
            [interface_.aecp removeResponseHandlerForControllerEntityID:controller_id_.to_uint64()];
        }
        if (bridge_ != nil) {
            bridge_->owner.store(nullptr);
        }
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return valid_; }

    // ---- Lifecycle ---------------------------------------------------------

    void set_sink(ControllerServiceSink sink) override { sink_ = std::move(sink); }

    void start() override
    {
        if (!valid_) {
            return;
        }
        // Unsolicited AEM notifications route through the AECP client
        // handler registered for our controller entity id.
        [interface_.aecp setResponseHandler:bridge_ forControllerEntityID:controller_id_.to_uint64()];
        [interface_.entityDiscovery primeIterators];
        (void)[interface_.entityDiscovery discoverEntities];
    }

    void tick(int64_t /*now_ns*/) override
    {
        // The framework owns all protocol timing.
    }

    [[nodiscard]] auto pollable() noexcept -> net::Pollable* override { return marshal_.get(); }

    // ---- Discovery ---------------------------------------------------------

    void discover_all() override
    {
        if (valid_) {
            (void)[interface_.entityDiscovery discoverEntities];
        }
    }

    [[nodiscard]] auto find_entity(Eui64 const& id) const -> DiscoveredEntity const* override
    {
        auto const it = entities_.find(id);
        return (it != entities_.end()) ? &it->second : nullptr;
    }

    // ---- AEM ---------------------------------------------------------------

    auto send_aem_command(
        Eui64 const& target, uint16_t command_code, std::span<uint8_t const> payload, AemCommandCompletion completion)
        -> bool override
    {
        auto const it = entities_.find(target);
        if (!valid_ || it == entities_.end()) {
            // Unknown entity / dead framework session: the outcome still
            // arrives through the completion, exactly once.
            fail_send(target, command_code, payload, std::move(completion));
            return true;
        }

        AVB17221AECPAEMMessage* msg = [AVB17221AECPAEMMessage commandMessage];
        msg.commandType = static_cast<AVB17221AEMCommandType>(command_code);
        msg.targetEntityID = target.to_uint64();
        msg.controllerEntityID = controller_id_.to_uint64();
        if (!payload.empty()) {
            msg.commandSpecificData = [NSData dataWithBytes:payload.data() length:payload.size()];
        }
        AVBMACAddress* mac = [[AVBMACAddress alloc] initWithBytes:it->second.source_mac.span().data()];

        AemOutcome seed{};
        seed.command_type = command_code;
        seed.target = target;
        auto const sent_len = std::min(payload.size(), seed.sent.capacity());
        seed.sent.assign(payload.begin(), payload.begin() + static_cast<ptrdiff_t>(sent_len));

        auto alive = alive_;
        auto marshal = marshal_;
        auto* self = this;
        ++inflight_;
        BOOL const ok = [interface_.aecp sendCommand:msg
                                        toMACAddress:mac
                                   completionHandler:^(NSError* error, AVB17221AECPMessage* response) {
                                     // Framework thread: capture the outcome as plain values.
                                     // The framework pairs the response with an NSError DERIVED
                                     // FROM THE AEM STATUS (AVBErrorDomain code 0 == SUCCESS), so
                                     // the response's presence — not a nil error — is the success
                                     // signal; a nil response with an IOReturn-style error code is
                                     // the transport timeout.
                                     (void)error;
                                     AemOutcome outcome = seed;
                                     if (response != nil) {
                                         outcome.delivery = AemCommandDelivery::Responded;
                                         outcome.status = static_cast<uint8_t>(response.status);
                                         if ([response isKindOfClass:[AVB17221AECPAEMMessage class]]) {
                                             NSData* data = ((AVB17221AECPAEMMessage*)response).commandSpecificData;
                                             if (data != nil) {
                                                 auto const n = std::min<size_t>(data.length, outcome.response.capacity());
                                                 auto const* bytes = static_cast<uint8_t const*>(data.bytes);
                                                 outcome.response.assign(bytes, bytes + n);
                                             }
                                         }
                                     } else {
                                         outcome.delivery = AemCommandDelivery::TimedOut;
                                     }
                                     marshal->post([alive, self, outcome, completion]() {
                                         if (!alive->load()) {
                                             return;
                                         }
                                         self->deliver_aem_outcome(outcome, completion);
                                     });
                                   }];
        if (ok == NO) {
            --inflight_;
            fail_send(target, command_code, payload, std::move(completion));
        }
        return true;
    }

    [[nodiscard]] auto aem_inflight_count() const -> size_t override { return inflight_.load(); }

    auto read_descriptor(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        std::array<uint8_t, 8> payload{};  // configuration 0 + reserved + descriptor header
        put_u16(payload, 4, desc_type);
        put_u16(payload, 6, desc_index);
        return send_aem_command(target, AEM_COMMAND_READ_DESCRIPTOR, payload, {});
    }

    auto set_identify(Eui64 const& target, bool on, AemCommandCompletion completion) -> bool override
    {
        auto const* entity = find_entity(target);
        if (entity == nullptr ||
            !entity->adpdu.has_entity_capability(atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID)) {
            return false;
        }
        uint16_t const control_index = entity->adpdu.identify_control_index.get();
        std::array<uint8_t, 5> payload{};
        put_u16(payload, 0, aem::DESCRIPTOR_CONTROL);
        put_u16(payload, 2, control_index);
        payload[4] = on ? 0xFF : 0x00;
        return send_aem_command(target, AEM_COMMAND_SET_CONTROL, payload, std::move(completion));
    }

    auto get_counters(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        return send_aem_command(target, AEM_COMMAND_GET_COUNTERS, desc_header(desc_type, desc_index), {});
    }

    auto set_stream_format(Eui64 const& target, uint16_t desc_type, uint16_t desc_index, uint64_t stream_format) -> bool override
    {
        std::array<uint8_t, 12> payload{};
        put_u16(payload, 0, desc_type);
        put_u16(payload, 2, desc_index);
        (void)ieee::store_unchecked(std::span<uint8_t>{payload}.subspan(4), Eui64{}.from_uint64(stream_format));
        return send_aem_command(target, AEM_COMMAND_SET_STREAM_FORMAT, payload, {});
    }

    auto start_streaming(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        return send_aem_command(target, AEM_COMMAND_START_STREAMING, desc_header(desc_type, desc_index), {});
    }

    auto stop_streaming(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        return send_aem_command(target, AEM_COMMAND_STOP_STREAMING, desc_header(desc_type, desc_index), {});
    }

    auto set_clock_source(Eui64 const& target, uint16_t desc_index, uint16_t clock_source_index, AemCommandCompletion completion)
        -> bool override
    {
        std::array<uint8_t, 8> payload{};
        put_u16(payload, 0, aem::DESCRIPTOR_CLOCK_DOMAIN);
        put_u16(payload, 2, desc_index);
        put_u16(payload, 4, clock_source_index);
        return send_aem_command(target, AEM_COMMAND_SET_CLOCK_SOURCE, payload, std::move(completion));
    }

    auto get_clock_source(Eui64 const& target, uint16_t desc_index, AemCommandCompletion completion) -> bool override
    {
        std::array<uint8_t, 4> payload{};
        put_u16(payload, 0, aem::DESCRIPTOR_CLOCK_DOMAIN);
        put_u16(payload, 2, desc_index);
        return send_aem_command(target, AEM_COMMAND_GET_CLOCK_SOURCE, payload, std::move(completion));
    }

    auto set_signal_selector(
        Eui64 const& target,
        uint16_t desc_index,
        uint16_t signal_type,
        uint16_t signal_index,
        uint16_t signal_output,
        AemCommandCompletion completion) -> bool override
    {
        std::array<uint8_t, 12> payload{};  // trailing reserved doublet stays zero
        put_u16(payload, 0, aem::DESCRIPTOR_SIGNAL_SELECTOR);
        put_u16(payload, 2, desc_index);
        put_u16(payload, 4, signal_type);
        put_u16(payload, 6, signal_index);
        put_u16(payload, 8, signal_output);
        return send_aem_command(target, AEM_COMMAND_SET_SIGNAL_SELECTOR, payload, std::move(completion));
    }

    auto get_signal_selector(Eui64 const& target, uint16_t desc_index, AemCommandCompletion completion) -> bool override
    {
        std::array<uint8_t, 4> payload{};
        put_u16(payload, 0, aem::DESCRIPTOR_SIGNAL_SELECTOR);
        put_u16(payload, 2, desc_index);
        return send_aem_command(target, AEM_COMMAND_GET_SIGNAL_SELECTOR, payload, std::move(completion));
    }

    // ---- ACMP ---------------------------------------------------------------

    auto connect_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return send_acmp(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, talker, talker_uid, listener, listener_uid);
    }

    auto disconnect_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return send_acmp(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, talker, talker_uid, listener, listener_uid);
    }

    auto connect_tx_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return send_acmp(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, talker, talker_uid, listener, listener_uid);
    }

    auto disconnect_tx_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid)
        -> bool override
    {
        return send_acmp(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, talker, talker_uid, listener, listener_uid);
    }

    auto get_rx_state(Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return send_acmp(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, Eui64{}, 0, listener, listener_uid);
    }

    auto get_tx_state(Eui64 const& talker, uint16_t talker_uid) -> bool override
    {
        return send_acmp(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, talker, talker_uid, Eui64{}, 0);
    }

    // ---- Reactor-thread appliers (invoked by tasks the bridge posts) --------

    /// Reactor thread: fold an entity snapshot into the directory and
    /// notify the sink.
    void apply_entity_event(EntitySnapshot const& snap, bool const removed)
    {
        auto const id = eui64_of(snap.entity_id);
        if (removed) {
            entities_.erase(id);
            if (sink_.on_entity_departed) {
                sink_.on_entity_departed(id);
            }
            return;
        }
        bool const existed = entities_.contains(id);
        auto& rec = entities_[id];
        rec.adpdu.entity_id = id;
        rec.adpdu.entity_model_id = eui64_of(snap.entity_model_id);
        rec.adpdu.entity_capabilities = snap.entity_capabilities;
        rec.adpdu.talker_stream_sources = snap.talker_stream_sources;
        rec.adpdu.talker_capabilities = snap.talker_capabilities;
        rec.adpdu.listener_stream_sinks = snap.listener_stream_sinks;
        rec.adpdu.listener_capabilities = snap.listener_capabilities;
        rec.adpdu.controller_capabilities = snap.controller_capabilities;
        rec.adpdu.available_index = snap.available_index;
        rec.adpdu.gptp_grandmaster_id = tsn::ClockIdentity{snap.gptp_grandmaster_id};
        rec.adpdu.gptp_domain_number = snap.gptp_domain_number;
        rec.adpdu.identify_control_index = snap.identify_control_index;
        rec.adpdu.interface_index = snap.interface_index;
        rec.adpdu.association_id = eui64_of(snap.association_id);
        rec.source_mac = snap.mac;
        rec.valid = true;
        if (!existed) {
            if (sink_.on_entity_added) {
                sink_.on_entity_added(rec);
            }
        } else if (sink_.on_entity_updated) {
            sink_.on_entity_updated(rec);
        }
    }

    /// Reactor thread: surface an unsolicited AEM notification on the
    /// broadcast response sink.
    void deliver_unsolicited(AemOutcome const& outcome)
    {
        if (sink_.on_aem_response) {
            sink_.on_aem_response(
                outcome.target,
                outcome.command_type,
                outcome.status,
                std::span<uint8_t const>{},
                std::span<uint8_t const>{outcome.response.data(), outcome.response.size()});
        }
    }

  private:
    static auto desc_header(uint16_t desc_type, uint16_t desc_index) -> std::array<uint8_t, 4>
    {
        std::array<uint8_t, 4> out{};
        put_u16(out, 0, desc_type);
        put_u16(out, 2, desc_index);
        return out;
    }

    /// Fire the SendFailed outcome for a command that never reached the
    /// framework (unknown entity, framework refusal).
    void fail_send(Eui64 const& target, uint16_t command_code, std::span<uint8_t const> payload, AemCommandCompletion completion)
    {
        if (!completion) {
            return;
        }
        completion(AemCommandResult{
            .delivery = AemCommandDelivery::SendFailed,
            .status = 0,
            .command_type = command_code,
            .target_entity_id = target,
            .sent_payload = payload,
            .response = {}});
    }

    /// Reactor thread: deliver one AEM outcome to the broadcast sink and
    /// the per-command completion, mirroring the raw backend's ordering.
    void deliver_aem_outcome(AemOutcome const& outcome, AemCommandCompletion const& completion)
    {
        --inflight_;
        std::span<uint8_t const> const sent{outcome.sent.data(), outcome.sent.size()};
        std::span<uint8_t const> const response{outcome.response.data(), outcome.response.size()};
        if (outcome.delivery == AemCommandDelivery::Responded) {
            if (sink_.on_aem_response) {
                sink_.on_aem_response(outcome.target, outcome.command_type, outcome.status, sent, response);
            }
        } else if (sink_.on_aem_timeout) {
            sink_.on_aem_timeout(outcome.target, outcome.command_type);
        }
        if (completion) {
            completion(AemCommandResult{
                .delivery = outcome.delivery,
                .status = outcome.status,
                .command_type = outcome.command_type,
                .target_entity_id = outcome.target,
                .sent_payload = sent,
                .response = response});
        }
    }

    auto send_acmp(uint8_t message_type, Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid)
        -> bool
    {
        if (!valid_) {
            return false;
        }
        AVB17221ACMPMessage* msg = [[AVB17221ACMPMessage alloc] init];
        msg.messageType = static_cast<AVB17221ACMPMessageType>(message_type);
        msg.controllerEntityID = controller_id_.to_uint64();
        msg.talkerEntityID = talker.to_uint64();
        msg.talkerUniqueID = talker_uid;
        msg.listenerEntityID = listener.to_uint64();
        msg.listenerUniqueID = listener_uid;

        auto alive = alive_;
        auto marshal = marshal_;
        auto* self = this;
        BOOL const ok = [interface_.acmp
            sendACMPCommandMessage:msg
                 completionHandler:^(NSError* error, AVB17221ACMPMessage* response) {
                   // Framework thread: capture into our decoded PDU. As with
                   // AECP, the NSError mirrors the ACMP status; only a nil
                   // response means the exchange timed out.
                   (void)error;
                   AcmpDu pdu{};
                   bool const timed_out = (response == nil);
                   if (!timed_out) {
                       pdu.init_response(static_cast<uint8_t>(response.messageType), static_cast<uint8_t>(response.status));
                       pdu.stream_id.from_uint64(response.streamID);
                       pdu.controller_entity_id.from_uint64(response.controllerEntityID);
                       pdu.talker_entity_id.from_uint64(response.talkerEntityID);
                       pdu.talker_unique_id = response.talkerUniqueID;
                       pdu.listener_entity_id.from_uint64(response.listenerEntityID);
                       pdu.listener_unique_id = response.listenerUniqueID;
                       pdu.connection_count = response.connectionCount;
                       pdu.flags = static_cast<uint16_t>(response.flags);
                       pdu.stream_vlan_id = response.vlanID;
                   } else {
                       pdu.init_command(static_cast<uint8_t>(message_type));
                       pdu.talker_entity_id = talker;
                       pdu.talker_unique_id = talker_uid;
                       pdu.listener_entity_id = listener;
                       pdu.listener_unique_id = listener_uid;
                   }
                   marshal->post([alive, self, pdu, timed_out]() {
                       if (!alive->load()) {
                           return;
                       }
                       self->deliver_acmp(pdu, timed_out);
                   });
                 }];
        if (ok == NO) {
            // Framework refused the send: the completion block will never
            // fire, so synthesize the timeout outcome ourselves — the sink
            // sees exactly one outcome per command on this backend too.
            AcmpDu pdu{};
            pdu.init_command(message_type);
            pdu.talker_entity_id = talker;
            pdu.talker_unique_id = talker_uid;
            pdu.listener_entity_id = listener;
            pdu.listener_unique_id = listener_uid;
            marshal->post([alive, self, pdu]() {
                if (!alive->load()) {
                    return;
                }
                self->deliver_acmp(pdu, /*timed_out=*/true);
            });
        }
        return true;
    }

    /// Reactor thread: an ACMP outcome. Responses feed both the response
    /// sink and the passive observation feed (this backend cannot see
    /// third-party ACMP traffic, but observing our own responses keeps
    /// connection tracking alive for everything we initiate).
    void deliver_acmp(AcmpDu const& pdu, bool const timed_out)
    {
        auto const resp = acmp_command_response_from_pdu(pdu);
        if (timed_out) {
            if (sink_.on_acmp_timeout) {
                sink_.on_acmp_timeout(resp);
            }
            return;
        }
        if (sink_.on_acmp_observed) {
            sink_.on_acmp_observed(pdu);
        }
        if (sink_.on_acmp_response) {
            sink_.on_acmp_response(resp);
        }
    }

    Eui64 controller_id_{};
    // Shared with the bridge and every framework completion block, so late
    // callbacks can post safely after this service is destroyed.
    std::shared_ptr<ReactorMarshal> marshal_{std::make_shared<ReactorMarshal>()};
    AVBEthernetInterface* interface_{nil};
    StatusbarAvbBridge* bridge_{nil};
    ControllerServiceSink sink_{};
    std::map<Eui64, DiscoveredEntity> entities_;  // reactor thread only
    std::atomic<size_t> inflight_{0};
    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};
    bool valid_{false};
};

auto make_macos_avb_controller_service(std::string const& interface_name, ieee::Eui64 controller_id)
    -> std::unique_ptr<ControllerService>
{
    auto service = std::make_unique<MacAvbControllerService>(interface_name, controller_id);
    if (!service->valid()) {
        return nullptr;
    }
    return service;
}

}  // namespace statusbar::atdecc_tools

// ---- Objective-C bridge ----------------------------------------------------

@implementation StatusbarAvbBridge

/// Framework thread: post a guarded task that applies @p snap on the
/// reactor thread. `owner` is only captured by value here; the task
/// dereferences it only after the alive check on the reactor thread.
- (void)postEntitySnapshot:(statusbar::atdecc_tools::EntitySnapshot const&)snap removed:(bool)removed
{
    auto alive_copy = alive;
    auto marshal_copy = marshal;
    auto* svc = owner.load();
    if (alive_copy == nullptr || marshal_copy == nullptr || svc == nullptr) {
        return;
    }
    marshal_copy->post([alive_copy, svc, snap, removed]() {
        if (!alive_copy->load()) {
            return;
        }
        svc->apply_entity_event(snap, removed);
    });
}

- (void)didAddRemoteEntity:(AVB17221Entity*)newEntity on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(newEntity) removed:false];
}

- (void)didRemoveRemoteEntity:(AVB17221Entity*)oldEntity on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(oldEntity) removed:true];
}

- (void)didRediscoverRemoteEntity:(AVB17221Entity*)entity on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(entity) removed:false];
}

- (void)didUpdateRemoteEntity:(AVB17221Entity*)entity
            changedProperties:(AVB17221EntityPropertyChanged)changedProperties
       on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(entity) removed:false];
}

- (void)didAddLocalEntity:(AVB17221Entity*)newEntity on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(newEntity) removed:false];
}

- (void)didRemoveLocalEntity:(AVB17221Entity*)oldEntity on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(oldEntity) removed:true];
}

- (void)didRediscoverLocalEntity:(AVB17221Entity*)entity on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(entity) removed:false];
}

- (void)didUpdateLocalEntity:(AVB17221Entity*)entity
           changedProperties:(AVB17221EntityPropertyChanged)changedProperties
      on17221EntityDiscovery:(AVB17221EntityDiscovery*)entityDiscovery
{
    [self postEntitySnapshot:statusbar::atdecc_tools::snapshot_of(entity) removed:false];
}

- (BOOL)AECPDidReceiveCommand:(AVB17221AECPMessage*)message onInterface:(AVB17221AECPInterface*)anInterface
{
    // Controller-only backend: we answer no inbound AEM commands.
    return NO;
}

- (BOOL)AECPDidReceiveResponse:(AVB17221AECPMessage*)message onInterface:(AVB17221AECPInterface*)anInterface
{
    // Responses matched to our commands arrive via completion handlers;
    // forward only unsolicited AEM notifications.
    if (![message isKindOfClass:[AVB17221AECPAEMMessage class]]) {
        return NO;
    }
    AVB17221AECPAEMMessage* aem = (AVB17221AECPAEMMessage*)message;
    if (!aem.isUnsolicited) {
        return NO;
    }
    auto alive_copy = alive;
    auto marshal_copy = marshal;
    auto* svc = owner.load();
    if (alive_copy == nullptr || marshal_copy == nullptr || svc == nullptr) {
        return NO;
    }
    statusbar::atdecc_tools::AemOutcome outcome{};
    outcome.delivery = statusbar::atdecc::AemCommandDelivery::Responded;
    outcome.status = static_cast<uint8_t>(aem.status);
    outcome.command_type = static_cast<uint16_t>(aem.commandType);
    outcome.target = statusbar::atdecc_tools::eui64_of(aem.targetEntityID);
    if (aem.commandSpecificData != nil) {
        auto const n = std::min<size_t>(aem.commandSpecificData.length, outcome.response.capacity());
        auto const* bytes = static_cast<uint8_t const*>(aem.commandSpecificData.bytes);
        outcome.response.assign(bytes, bytes + n);
    }
    marshal_copy->post([alive_copy, svc, outcome]() {
        if (!alive_copy->load()) {
            return;
        }
        svc->deliver_unsolicited(outcome);
    });
    return YES;
}

@end
