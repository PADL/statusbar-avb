// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_aem_entity_model.hpp"

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace statusbar::nanoavb {

using atdecc::AEM_STATUS_BAD_ARGUMENTS;
using atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
using atdecc::AEM_STATUS_SUCCESS;
using atdecc::aem::doublet_t;
using namespace atdecc::aem;

namespace {

/// Per-type dispatch wrapper signature.
///
/// Each wrapper materializes a stack-local descriptor struct, optionally
/// preloads it from the DescriptorStorage blob via span_load_padded,
/// calls the handler, and writes wire_size() bytes to `out` via
/// span_store_wire. Returns the number of bytes written, 0 on any failure.
using GetWireFn = size_t (*)(
    AemEntityHandler& handler, DescriptorRef ref, uint32_t symbol, DescriptorStorage const* storage, std::span<uint8_t> out);

/// Template dispatch wrapper. The caller instantiates one specialization
/// per (descriptor C++ struct type, handler method) pair; the dispatch
/// table stores the resulting function pointer.
///
/// Notes on preloading: span_load_padded zero-fills any bytes beyond the
/// source blob's length and copies up to sizeof(T). For variable-length
/// descriptors whose inline trailer was built into the struct during
/// Phase 2, this correctly populates BOTH the fixed header and any
/// populated trailer entries. For fixed descriptors the preload
/// is just a plain sizeof(T) copy.
template <typename T, auto HandlerMethod>
auto dispatch_fixed(
    AemEntityHandler& handler, DescriptorRef ref, uint32_t symbol, DescriptorStorage const* storage, std::span<uint8_t> out)
    -> size_t
{
    T desc{};

    // Preload the static fields from the DescriptorStorage blob if one
    // is attached and the (config, type, index) triple is present.
    if (storage != nullptr) {
        auto blob_result = storage->get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (blob_result.has_value()) {
            span_load_padded(desc, *blob_result);
        }
    }

    // Ensure the descriptor_type field reflects the requested variant
    // (for paired descriptors like STREAM_INPUT / STREAM_OUTPUT, the
    // handler sees the same C++ struct but different wire-type codes).
    desc.descriptor_type = ref.descriptor_type;
    desc.descriptor_index = ref.descriptor_index;

    // Delegate to the handler for dynamic fields.
    if (!(handler.*HandlerMethod)(DescriptorId{.ref = ref, .symbol = symbol}, desc)) {
        return 0;
    }

    // A variable-length descriptor's trailer count (formats, sampling rates,
    // descriptor_counts, ...) is loaded straight from the on-disk .aem blob and
    // may be corrupt, or larger than this (possibly older) build's struct can
    // hold because a future standard grew the descriptor. Clamp it to the struct
    // capacity so we serve a VALID, truncated descriptor (keep what fits, ignore
    // the excess) instead of over-reading the struct in span_store_wire below --
    // the same graceful clamping the receive/parse side already does via used_*().
    if constexpr (requires { desc.clamp_trailer_count(); }) {
        desc.clamp_trailer_count();
    }
    auto const n = desc.wire_size();
    if (out.size() < n) {
        return 0;
    }
    span_store_wire(out, desc);
    return n;
}

/// Dispatch table size. AEM descriptor_type codes occupy 0x0000..~0x0027
/// in IEEE 1722.1, plus the 2016/2021 extensions. 0x80 leaves plenty of
/// headroom without bloating the table.
constexpr size_t DISPATCH_TABLE_SIZE = 0x80;

/// Build the get-descriptor dispatch table at compile time. Each entry
/// routes a specific DESCRIPTOR_* code to the appropriate
/// dispatch_fixed<T, &AemEntityHandler::on_get_X> instantiation.
///
/// Input/output pairs (STREAM_INPUT/OUTPUT, JACK_INPUT/OUTPUT, etc.)
/// point to the SAME handler method but different table slots, so the
/// handler branches on ref.descriptor_type to distinguish direction.
[[nodiscard]] constexpr auto build_get_dispatch_table() -> std::array<GetWireFn, DISPATCH_TABLE_SIZE>
{
    std::array<GetWireFn, DISPATCH_TABLE_SIZE> table{};

    table[DESCRIPTOR_ENTITY] = &dispatch_fixed<DescriptorEntity, &AemEntityHandler::on_get_entity>;
    table[DESCRIPTOR_CONFIGURATION] = &dispatch_fixed<DescriptorConfiguration, &AemEntityHandler::on_get_configuration>;
    table[DESCRIPTOR_AUDIO_UNIT] = &dispatch_fixed<DescriptorAudioUnit, &AemEntityHandler::on_get_audio_unit>;
    table[DESCRIPTOR_VIDEO_UNIT] = &dispatch_fixed<DescriptorVideoUnit, &AemEntityHandler::on_get_video_unit>;
    table[DESCRIPTOR_SENSOR_UNIT] = &dispatch_fixed<DescriptorSensorUnit, &AemEntityHandler::on_get_sensor_unit>;
    table[DESCRIPTOR_STREAM_INPUT] = &dispatch_fixed<DescriptorStream, &AemEntityHandler::on_get_stream>;
    table[DESCRIPTOR_STREAM_OUTPUT] = &dispatch_fixed<DescriptorStream, &AemEntityHandler::on_get_stream>;
    table[DESCRIPTOR_JACK_INPUT] = &dispatch_fixed<DescriptorJack, &AemEntityHandler::on_get_jack>;
    table[DESCRIPTOR_JACK_OUTPUT] = &dispatch_fixed<DescriptorJack, &AemEntityHandler::on_get_jack>;
    table[DESCRIPTOR_AVB_INTERFACE] = &dispatch_fixed<DescriptorAvbInterface, &AemEntityHandler::on_get_avb_interface>;
    table[DESCRIPTOR_CLOCK_SOURCE] = &dispatch_fixed<DescriptorClockSource, &AemEntityHandler::on_get_clock_source>;
    table[DESCRIPTOR_MEMORY_OBJECT] = &dispatch_fixed<DescriptorMemoryObject, &AemEntityHandler::on_get_memory_object>;
    table[DESCRIPTOR_LOCALE] = &dispatch_fixed<DescriptorLocale, &AemEntityHandler::on_get_locale>;
    table[DESCRIPTOR_STRINGS] = &dispatch_fixed<DescriptorStrings, &AemEntityHandler::on_get_strings>;
    table[DESCRIPTOR_STREAM_PORT_INPUT] = &dispatch_fixed<DescriptorStreamPort, &AemEntityHandler::on_get_stream_port>;
    table[DESCRIPTOR_STREAM_PORT_OUTPUT] = &dispatch_fixed<DescriptorStreamPort, &AemEntityHandler::on_get_stream_port>;
    table[DESCRIPTOR_EXTERNAL_PORT_INPUT] = &dispatch_fixed<DescriptorExternalPort, &AemEntityHandler::on_get_external_port>;
    table[DESCRIPTOR_EXTERNAL_PORT_OUTPUT] = &dispatch_fixed<DescriptorExternalPort, &AemEntityHandler::on_get_external_port>;
    table[DESCRIPTOR_INTERNAL_PORT_INPUT] = &dispatch_fixed<DescriptorInternalPort, &AemEntityHandler::on_get_internal_port>;
    table[DESCRIPTOR_INTERNAL_PORT_OUTPUT] = &dispatch_fixed<DescriptorInternalPort, &AemEntityHandler::on_get_internal_port>;
    table[DESCRIPTOR_AUDIO_CLUSTER] = &dispatch_fixed<DescriptorAudioCluster, &AemEntityHandler::on_get_audio_cluster>;
    table[DESCRIPTOR_VIDEO_CLUSTER] = &dispatch_fixed<DescriptorVideoCluster, &AemEntityHandler::on_get_video_cluster>;
    table[DESCRIPTOR_SENSOR_CLUSTER] = &dispatch_fixed<DescriptorSensorCluster, &AemEntityHandler::on_get_sensor_cluster>;
    table[DESCRIPTOR_AUDIO_MAP] = &dispatch_fixed<DescriptorAudioMap, &AemEntityHandler::on_get_audio_map>;
    table[DESCRIPTOR_VIDEO_MAP] = &dispatch_fixed<DescriptorVideoMap, &AemEntityHandler::on_get_video_map>;
    table[DESCRIPTOR_SENSOR_MAP] = &dispatch_fixed<DescriptorSensorMap, &AemEntityHandler::on_get_sensor_map>;
    table[DESCRIPTOR_CONTROL] = &dispatch_fixed<DescriptorControl, &AemEntityHandler::on_get_control>;
    table[DESCRIPTOR_SIGNAL_SELECTOR] = &dispatch_fixed<DescriptorSignalSelector, &AemEntityHandler::on_get_signal_selector>;
    table[DESCRIPTOR_MIXER] = &dispatch_fixed<DescriptorMixer, &AemEntityHandler::on_get_mixer>;
    table[DESCRIPTOR_MATRIX] = &dispatch_fixed<DescriptorMatrix, &AemEntityHandler::on_get_matrix>;
    table[DESCRIPTOR_MATRIX_SIGNAL] = &dispatch_fixed<DescriptorMatrixSignal, &AemEntityHandler::on_get_matrix_signal>;
    table[DESCRIPTOR_SIGNAL_SPLITTER] = &dispatch_fixed<DescriptorSignalSplitter, &AemEntityHandler::on_get_signal_splitter>;
    table[DESCRIPTOR_SIGNAL_COMBINER] = &dispatch_fixed<DescriptorSignalCombiner, &AemEntityHandler::on_get_signal_combiner>;
    table[DESCRIPTOR_SIGNAL_DEMULTIPLEXER] =
        &dispatch_fixed<DescriptorSignalDemultiplexer, &AemEntityHandler::on_get_signal_demultiplexer>;
    table[DESCRIPTOR_SIGNAL_MULTIPLEXER] =
        &dispatch_fixed<DescriptorSignalMultiplexer, &AemEntityHandler::on_get_signal_multiplexer>;
    table[DESCRIPTOR_SIGNAL_TRANSCODER] = &dispatch_fixed<DescriptorSignalTranscoder, &AemEntityHandler::on_get_signal_transcoder>;
    table[DESCRIPTOR_CLOCK_DOMAIN] = &dispatch_fixed<DescriptorClockDomain, &AemEntityHandler::on_get_clock_domain>;
    table[DESCRIPTOR_CONTROL_BLOCK] = &dispatch_fixed<DescriptorControlBlock, &AemEntityHandler::on_get_control_block>;
    table[DESCRIPTOR_TIMING] = &dispatch_fixed<DescriptorTiming, &AemEntityHandler::on_get_timing>;
    table[DESCRIPTOR_PTP_INSTANCE] = &dispatch_fixed<DescriptorPtpInstance, &AemEntityHandler::on_get_ptp_instance>;
    table[DESCRIPTOR_PTP_PORT] = &dispatch_fixed<DescriptorPtpPort, &AemEntityHandler::on_get_ptp_port>;

    return table;
}

constexpr auto GET_DISPATCH_TABLE = build_get_dispatch_table();

/// GET_NAME / SET_NAME response body size per IEEE 1722.1 Clause 7.4.18.1:
///   descriptor_type:2 + descriptor_index:2 + name_index:2 + configuration_index:2 + name:64 = 72
constexpr size_t NAME_RESPONSE_SIZE = 72;
constexpr size_t NAME_HEADER_SIZE = 8;
static_assert(NAME_RESPONSE_SIZE == NAME_HEADER_SIZE + AtdeccString::LENGTH);

}  // namespace

auto AemEntityModel::symbol_for(DescriptorRef ref) const -> uint32_t
{
    // Resolve the symbol from this model's own attached storage if it has one,
    // otherwise from the handler's backing storage (DescriptorStorageHandler and
    // friends). This lets symbols resolve through the normal command path even when
    // the per-command AemEntityModel is built handler-only (the usual case).
    DescriptorStorage const* const store = storage_ ? &*storage_ : handler_->descriptor_storage();
    if (store == nullptr) {
        return 0;
    }
    auto result = store->get_symbol(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
    if (!result) {
        return 0;
    }
    return *result;
}

auto AemEntityModel::get_descriptor_for_wire(DescriptorRef ref, std::span<uint8_t> out) const -> size_t
{
    if (ref.descriptor_type >= GET_DISPATCH_TABLE.size()) {
        return 0;
    }
    auto const fn = GET_DISPATCH_TABLE[ref.descriptor_type];
    if (fn == nullptr) {
        return 0;
    }
    return fn(*handler_, ref, symbol_for(ref), static_store(), out);
}

auto AemEntityModel::get_name_for_wire(NameRef ref, std::span<uint8_t> out) const -> size_t
{
    if (out.size() < NAME_RESPONSE_SIZE) {
        return 0;
    }

    auto name_opt =
        handler_->on_get_name(DescriptorId{.ref = ref.descriptor, .symbol = symbol_for(ref.descriptor)}, ref.name_index);
    if (!name_opt) {
        return 0;
    }

    // Serialize GET_NAME response body: 8-byte header + 64-byte name.
    doublet_t const dtype{ref.descriptor.descriptor_type};
    doublet_t const dindex{ref.descriptor.descriptor_index};
    doublet_t const nindex{ref.name_index};
    doublet_t const cindex{ref.descriptor.configuration_index};

    span_store(out.subspan(0, 2), dtype);
    span_store(out.subspan(2, 2), dindex);
    span_store(out.subspan(4, 2), nindex);
    span_store(out.subspan(6, 2), cindex);
    span_copy(out.subspan(NAME_HEADER_SIZE, AtdeccString::LENGTH), make_const_span(*name_opt));
    return NAME_RESPONSE_SIZE;
}

auto AemEntityModel::apply_set_name(std::span<uint8_t const> command_body) -> uint8_t
{
    if (command_body.size() < NAME_RESPONSE_SIZE) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }

    // Parse the 8-byte SET_NAME command header. Layout matches the
    // GET_NAME response header above: type, index, name_index, config.
    doublet_t dtype{};
    doublet_t dindex{};
    doublet_t nindex{};
    doublet_t cindex{};
    span_load(dtype, command_body.subspan(0, 2));
    span_load(dindex, command_body.subspan(2, 2));
    span_load(nindex, command_body.subspan(4, 2));
    span_load(cindex, command_body.subspan(6, 2));

    NameRef const ref{
        .descriptor =
            DescriptorRef{.configuration_index = cindex.get(), .descriptor_type = dtype.get(), .descriptor_index = dindex.get()},
        .name_index = nindex.get()};

    // Extract the 64-byte name payload.
    AtdeccString name{};
    auto const name_bytes = command_body.subspan(NAME_HEADER_SIZE, AtdeccString::LENGTH);
    std::memcpy(name.value.data(), name_bytes.data(), AtdeccString::LENGTH);

    return handler_->on_set_name(DescriptorId{.ref = ref.descriptor, .symbol = symbol_for(ref.descriptor)}, ref.name_index, name);
}

auto AemEntityModel::apply_set_descriptor_value(
    uint16_t const command_type, std::span<uint8_t const> command_body, std::span<uint8_t> out) -> SetValueResult
{
    // Command body: descriptor_type(2) + descriptor_index(2) + value bytes.
    if (command_body.size() < 4) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    doublet_t dtype{};
    doublet_t dindex{};
    span_load(dtype, command_body.subspan(0, 2));
    span_load(dindex, command_body.subspan(2, 2));
    DescriptorRef const ref{.configuration_index = 0, .descriptor_type = dtype.get(), .descriptor_index = dindex.get()};

    auto const status = handler_->on_set_descriptor_value(
        command_type, DescriptorId{.ref = ref, .symbol = symbol_for(ref)}, command_body.subspan(4));

    // AECP echoes the SET command (descriptor_type/index + value) as the response body.
    size_t size = 0;
    if (out.size() >= command_body.size()) {
        std::copy(command_body.begin(), command_body.end(), out.begin());
        size = command_body.size();
    }
    return {.status = status, .size = size};
}

auto AemEntityModel::get_descriptor_value_for_wire(
    uint16_t const command_type, std::span<uint8_t const> command_body, std::span<uint8_t> out) const -> size_t
{
    // Command body: descriptor_type(2) + descriptor_index(2). Response echoes those,
    // then the handler's current value bytes.
    if (command_body.size() < 4 || out.size() < 4) {
        return 0;
    }
    doublet_t dtype{};
    doublet_t dindex{};
    span_load(dtype, command_body.subspan(0, 2));
    span_load(dindex, command_body.subspan(2, 2));
    DescriptorRef const ref{.configuration_index = 0, .descriptor_type = dtype.get(), .descriptor_index = dindex.get()};

    std::copy(command_body.begin(), command_body.begin() + 4, out.begin());
    auto const n =
        handler_->on_get_descriptor_value(command_type, DescriptorId{.ref = ref, .symbol = symbol_for(ref)}, out.subspan(4));
    if (n == 0) {
        return 0;  // no such descriptor / not implemented
    }
    return 4 + n;
}

}  // namespace statusbar::nanoavb
