#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ControllerSimple — a reusable ATDECC controller Pollable.
///
/// Wraps a NanoAvbAemController and a raw Ethernet socket (RawnetContext),
/// tracking discovered entities, per-stream metadata (object_name, stream
/// format), active stream connections (via passive ACMP observation), and
/// exposing the data in a form ready for ControllerApp::update_entities().
///
/// Designed for use in interactive tools such as the controller TUI, but
/// also reusable by other CLI tools that need an entity-level view of the
/// ATDECC network.

#include "statusbar/atdecc/atdecc_acmp_pdu.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc_tools/atdecc_aem_validate.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_model.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace statusbar::atdecc_tools {

/// Pollable ATDECC controller with entity and connection tracking
class ControllerSimple : public net::Pollable
{
  public:
    /// A pending GET_STREAM_FORMAT query tracked for retry
    struct PendingStreamFormatQuery
    {
        ieee::Eui64 target{};
        uint16_t desc_type{0};  ///< STREAM_INPUT or STREAM_OUTPUT
        uint16_t desc_index{0};
        int64_t last_sent_ns{0};  ///< Monotonic ns of last send, 0 if never sent
    };

    /// A pending READ_DESCRIPTOR work item. Every READ_DESCRIPTOR command
    /// flows through this queue (drained one per tick) so that no AEM
    /// command is sent synchronously from inside an AEM response callback,
    /// avoiding state-machine re-entrancy.
    struct PendingDescriptorRead
    {
        ieee::Eui64 target{};
        uint16_t descriptor_type{0};
        uint16_t descriptor_index{0};
        int64_t last_sent_ns{0};
    };

    /// Records a stream waiting for a STRINGS descriptor fetch to complete.
    struct StringsWaiter
    {
        uint16_t stream_desc_type{0};  ///< DESCRIPTOR_STREAM_INPUT/OUTPUT
        uint16_t stream_index{0};
        uint8_t string_offset{0};  ///< Which of the 7 strings to use (0-6)
    };

    ControllerSimple(net::RawnetContext context, ieee::Eui64 controller_id);

    // Pollable interface
    [[nodiscard]] auto fd() const noexcept -> int override;
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override;

    /// Access the underlying AEM controller (for lower-level commands).
    auto controller() -> nanoavb::NanoAvbAemController&;

    /// Build a fresh snapshot of discovered entities.
    auto get_display_entities() -> std::vector<EntityDisplayInfo>;

    /// Raw descriptors cached for an entity from the READ_DESCRIPTOR crawl
    /// (run fetch_entity_descriptors first and wait for EntityDetailReadyEvent).
    /// Only successfully-read descriptors are returned. For model validation.
    [[nodiscard]] auto cached_descriptors(ieee::Eui64 const& id) const -> std::vector<RawDescriptor>;

    /// Dispatch a controller action (from TUI, Lua, autonomous service, etc.).
    void dispatch(ControllerAction const& action, int64_t now_ns);

    /// Begin reading all descriptors for an entity.
    /// When complete, an EntityDetailReadyEvent will be emitted via drain_events().
    void fetch_entity_descriptors(ieee::Eui64 entity_id, int64_t now_ns);

    /// Queue GET_RX_STATE for all known listener entities (e.g. on refresh).
    /// No-op unless auto rx-state probing is enabled (see set_auto_probe_rx_state).
    void queue_rx_state_for_all();

    /// Enable/disable automatic GET_RX_STATE probing of every listener sink on
    /// entity discovery (and on DiscoverAll). OFF by default: a one-shot command
    /// like connect/list must not fan GET_RX_STATE at every sink on the LAN --
    /// an unresponsive multi-sink entity (e.g. a 18-sink the DSP processor) would peg the
    /// ACMP in-flight window and starve the actual CONNECT_RX_COMMAND. Live
    /// connection-tracking UIs (TUI) turn it ON to keep RX state fresh.
    void set_auto_probe_rx_state(bool on) noexcept { auto_probe_rx_state_ = on; }

    /// Enable/disable raw ACMP tracing. When ON, every observed ACMP PDU (command
    /// or response) surfaces as an AcmpTraceEvent via drain_events(), letting a
    /// diagnostic caller reconstruct a connect handshake leg by leg. OFF by
    /// default so normal callers (supervise/list) aren't flooded with bus traffic.
    void set_acmp_trace(bool on) noexcept { acmp_trace_ = on; }

    /// Immediately send one ACMP GET_RX_STATE to a specific listener sink. The
    /// reply (or its absence) surfaces as an RxStateEvent via drain_events().
    void query_rx_state(ieee::Eui64 listener_id, uint16_t unique_id, int64_t now_ns);

    /// Immediately send one ACMP GET_TX_STATE to a specific talker source (for
    /// comparing talker vs listener responsiveness during diagnosis).
    void query_tx_state(ieee::Eui64 talker_id, uint16_t unique_id, int64_t now_ns);

    /// Drain pending events (connection changes, detail ready, status, etc.).
    auto drain_events() -> std::vector<ControllerEvent>;

    /// Emit a status event from outside (e.g. tool-layer messages).
    void emit_status(std::string_view msg);

  private:
    void wire_controller();
    void forget_entity_metadata(ieee::Eui64 const& id);
    void queue_rx_state_for_entity(atdecc::AdpDu const& adp);
    auto make_active_connection(atdecc::AcmpDu const& acmp) -> ActiveConnection;
    void dispatch_frame(int64_t now_ns, ieee::Eui48 const& src_mac, std::span<uint8_t const> payload);
    void dispatch_adp(int64_t now_ns, ieee::Eui48 const& src_mac, std::span<uint8_t const> payload);
    void dispatch_acmp(std::span<uint8_t const> payload, int64_t now_ns);
    void handle_aem_response(
        ieee::Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data);
    void handle_read_descriptor_response(
        ieee::Eui64 target, uint8_t status, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data);
    void handle_stream_descriptor_response(
        ieee::Eui64 target, uint8_t status, uint16_t desc_type, uint16_t desc_index, std::span<uint8_t const> desc_payload);
    void handle_strings_descriptor_response(
        ieee::Eui64 target, uint8_t status, uint16_t strings_desc_idx, std::span<uint8_t const> desc_payload);
    void handle_get_stream_format_response(ieee::Eui64 target, uint8_t status, std::span<uint8_t const> data);
    void clear_pending_format_query(ieee::Eui64 const& target, uint16_t desc_type, uint16_t idx);
    /// Enqueue a GET_STREAM_FORMAT query. Deduplicates against the queue.
    void enqueue_stream_format_query(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index);

    /// Enqueue a READ_DESCRIPTOR work item. Deduplicates against the queue
    /// and against the per-entity already-read set. The actual send happens
    /// from tick().
    void enqueue_descriptor_read(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index);
    /// Remove a queued READ_DESCRIPTOR (called when the response arrives).
    void clear_descriptor_read(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index);
    /// Send the next eligible READ_DESCRIPTOR from the queue (one per tick).
    void send_next_descriptor_read(int64_t now_ns);
    /// Send the next eligible GET_STREAM_FORMAT (one per tick).
    void send_next_stream_format_query(int64_t now_ns);

    /// Parse a CONFIGURATION descriptor's descriptor_counts table and queue
    /// READ_DESCRIPTOR for every entry, in declaration order. Also queues
    /// GET_STREAM_FORMAT for stream descriptors.
    void handle_configuration_descriptor_response(ieee::Eui64 const& target, std::span<uint8_t const> desc_payload);

    net::RawnetContext context_;
    nanoavb::NanoAvbAemController controller_;
    std::array<uint8_t, 2048> payload_buf_{};

    // Auto GET_RX_STATE probing on discovery. OFF by default so one-shot commands
    // (connect/list) don't saturate the ACMP in-flight window; the TUI opts in.
    bool auto_probe_rx_state_{false};
    bool acmp_trace_{false};

    std::vector<ieee::Eui64> known_entity_ids_;

    // Streams waiting on a STRINGS descriptor fetch. Keyed by
    // {entity_id, strings_descriptor_index}. Multiple streams can reference
    // the same STRINGS descriptor, so each entry is a list.
    std::map<std::pair<ieee::Eui64, uint16_t>, std::vector<StringsWaiter>> strings_waiters_;

    // Cached STRINGS descriptor payloads, keyed by
    // {entity_id, strings_descriptor_index}. Populated by the STRINGS response
    // handler so that stream descriptors arriving AFTER the STRINGS descriptor
    // (the new normal under the unified CONFIGURATION-driven crawl) can
    // resolve their localized name from cached data instead of being stranded
    // with no waiter to fire.
    std::map<std::pair<ieee::Eui64, uint16_t>, std::vector<uint8_t>> strings_cache_;

    // Pending GET_STREAM_FORMAT queries — retry every STREAM_QUERY_RETRY_NS
    // until a response clears them.
    std::vector<PendingStreamFormatQuery> stream_format_queries_;
    size_t stream_format_cursor_{0};

    // Unified pending READ_DESCRIPTOR queue. EVERY READ_DESCRIPTOR command
    // (ENTITY, CONFIGURATION, STREAM_INPUT/OUTPUT, STRINGS, AVB_INTERFACE,
    // CLOCK_*, AUDIO_*, JACK_*, …) is enqueued here and drained one item
    // per tick. This avoids re-entering the AEM state machine from inside
    // a response callback and gives us deterministic ordering for the
    // full descriptor crawl.
    std::vector<PendingDescriptorRead> descriptor_read_queue_;
    size_t descriptor_read_cursor_{0};
    static constexpr int64_t STREAM_QUERY_RETRY_NS = 1'000'000'000;  // 1 second

    // Cached READ_DESCRIPTOR response bytes for one (type, index). Populated by
    // every READ_DESCRIPTOR response (success or failure) so the detail builder
    // can be seeded from already-fetched descriptors instead of re-issuing reads
    // the dedup set (EntityRecord::requested_descriptors) would skip.
    struct CachedDescriptor
    {
        bool success{false};
        std::vector<uint8_t> data;
    };

    // Entity detail builder for descriptor browsing.
    // Deduplicates responses by (descriptor_type, descriptor_index) so that
    // duplicate responses from the background stream query path don't corrupt
    // the detail view or cause premature completion.
    struct EntityDetailBuilder
    {
        ieee::Eui64 entity_id{};
        size_t expected{0};
        std::set<std::pair<uint16_t, uint16_t>> seen;
        std::vector<uint8_t> entity_desc_data;   // raw ENTITY descriptor payload
        std::vector<uint8_t> avb_iface_data;     // raw AVB_INTERFACE descriptor payload
        std::vector<uint8_t> clock_domain_data;  // raw CLOCK_DOMAIN descriptor payload
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> clock_source_descs;
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> stream_output_descs;
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> stream_input_descs;

        [[nodiscard]] auto is_complete() const noexcept -> bool { return seen.size() >= expected; }

        /// Record a response for this (type, index). Returns true if new, false if duplicate.
        /// Called once per response; pass status to indicate success vs. error.
        /// On success, stores the descriptor data. On error, the (type, index) is marked
        /// as received but no data is stored.
        void receive(uint16_t desc_type, uint16_t desc_index, bool success, std::span<uint8_t const> data);

        [[nodiscard]] auto build() const -> EntityDetail;
    };

    // GET_RX_STATE query queue: {listener_entity_id, unique_id}
    std::vector<std::pair<ieee::Eui64, uint16_t>> rx_state_query_queue_;

    // Queue of events ready to be drained by drain_events().
    std::vector<ControllerEvent> pending_events_;

    // Last seen available_index per entity. A new value strictly less than the
    // previous value indicates the entity rebooted (sequence reset). Kept SEPARATE
    // from EntityRecord because it has a distinct lifecycle: it must survive
    // forget_entity_metadata (a reboot forgets the metadata but keeps tracking the
    // index for the next comparison).
    std::map<ieee::Eui64, uint32_t> last_available_index_;

    // Per-entity actual descriptor counts from the entity's CONFIGURATION
    // descriptor. The ADPDU's talker_stream_sources / listener_stream_sinks
    // are upper bounds, not exact counts; the configuration's
    // descriptor_counts table is authoritative.
    struct EntityDescriptorCounts
    {
        uint16_t stream_inputs{0};
        uint16_t stream_outputs{0};
    };

    // All per-entity metadata that forget_entity_metadata() clears, gathered in one
    // record. Adding a field here can never leave a stale-data leak in
    // forget_entity_metadata (which is now a single entities_.erase). Keyed by
    // entity_id; the stream-indexed vectors are indexed by stream unique_id.
    struct EntityRecord
    {
        std::string name;
        std::vector<std::string> talker_formats;
        std::vector<std::string> listener_formats;
        std::vector<std::string> talker_stream_names;
        std::vector<std::string> listener_stream_names;
        // (descriptor_type, descriptor_index) pairs already enqueued or read (dedup).
        std::set<std::pair<uint16_t, uint16_t>> requested_descriptors;
        // Cached READ_DESCRIPTOR responses, keyed by (type, index).
        std::map<std::pair<uint16_t, uint16_t>, CachedDescriptor> descriptor_cache;
        // Present only while a detail build is in progress for this entity.
        std::optional<EntityDetailBuilder> detail_builder;
        bool identify_on{false};
        // Set once the CONFIGURATION descriptor is read; optional so a legitimate
        // {0,0} (controller-only entity) is distinguished from "not yet read".
        std::optional<EntityDescriptorCounts> descriptor_counts;
    };
    std::map<ieee::Eui64, EntityRecord> entities_;

    /// The record for @p id, or nullptr if the entity is unknown. Read-side helper
    /// for the many `if (found) use field` call sites.
    [[nodiscard]] auto find_entity(ieee::Eui64 const& id) const -> EntityRecord const*
    {
        auto const it = entities_.find(id);
        return (it != entities_.end()) ? &it->second : nullptr;
    }
};

}  // namespace statusbar::atdecc_tools
