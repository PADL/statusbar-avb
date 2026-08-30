#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB AEM Entity Model — handler-backed descriptor wire formatter.
///
/// This is the Phase-3 replacement for the vector-backed `EntityModel`.
/// Unlike the legacy model, AemEntityModel owns no descriptor storage;
/// it delegates every READ_DESCRIPTOR, GET_NAME, and SET_NAME request
/// to a user-supplied `AemEntityHandler` that generates descriptors
/// on demand. An optional `DescriptorStorage` may be attached to
/// preload static descriptor fields from a .aem blob before the handler
/// runs — handlers then need only fill in the dynamic fields.
///
/// Wire-format responsibilities
/// ----------------------------
/// The model is responsible for:
///   - Routing a request by descriptor_type to the correct handler method
///     via a constexpr dispatch table.
///   - Looking up the pre-agreed application symbol (if any) from the
///     DescriptorStorage symbol table and passing it through.
///   - Pre-populating a stack-local descriptor struct from the static
///     store via span_load_padded before calling the handler.
///   - Writing exactly `desc.wire_size()` bytes to the caller's output
///     buffer via span_store_wire.
///   - Framing GET_NAME responses as 8-byte header + 64-byte AtdeccString.
///
/// The model does NOT own descriptor memory — callers pass a
/// `std::span<uint8_t>` output buffer sized for MAX_AEM_DESCRIPTOR_SIZE.
/// The usual call site is AemCommandHandler::handle_read_descriptor,
/// which keeps one such buffer on the stack for the duration of a
/// single AECP command.
///
/// Threading
/// ---------
/// All methods must be called from the single reactor thread that owns
/// the handler. No internal locking is performed.

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::nanoavb {

using atdecc::aem::DescriptorStorage;

class AemEntityModel
{
  public:
    /// Construct with a handler alone. When the handler exposes a backing
    /// DescriptorStorage (descriptor_storage() non-null — the
    /// DescriptorStorageHandler family), the model self-wires to it, so
    /// blob preloads, symbol resolution, and raw-trailer serving (MATRIX /
    /// MATRIX_SIGNAL) work exactly as with the two-argument constructor —
    /// the command handler builds models through this constructor, and a
    /// null storage here silently truncated served MATRIX trailers.
    /// Otherwise the handler is fully responsible for populating every
    /// descriptor.
    explicit AemEntityModel(AemEntityHandler& handler) noexcept
        : handler_{&handler}
    {
        if (auto const* backing = handler.descriptor_storage()) {
            storage_ = *backing;
        }
    }

    /// Construct with a handler and a DescriptorStorage.
    /// Before each handler call, the model preloads the descriptor struct
    /// from the storage blob via span_load_padded — handlers then only
    /// need to fill in dynamic fields (state that is not known at build
    /// time), such as current stream format or active gain value.
    /// `DescriptorStorage` is a trivially copyable zero-copy view, so
    /// it's passed by value here.
    AemEntityModel(AemEntityHandler& handler, DescriptorStorage storage) noexcept
        : handler_{&handler}
        , storage_{storage}
    {}

    AemEntityModel(AemEntityModel const&) = delete;
    auto operator=(AemEntityModel const&) -> AemEntityModel& = delete;
    AemEntityModel(AemEntityModel&&) noexcept = default;
    auto operator=(AemEntityModel&&) noexcept -> AemEntityModel& = default;
    ~AemEntityModel() = default;

    /// The configuration index used for descriptor refs whose command
    /// payloads carry none on the wire (the SET/GET_*_VALUE dispatch).
    /// Defaults to configuration 0; the command handler sets the entity's
    /// current configuration here (kit phase 5c).
    void set_configuration(uint16_t const configuration) noexcept { configuration_ = configuration; }

    /// Populate `out` with the on-wire bytes of a single descriptor.
    /// Returns the number of bytes written, or 0 if the descriptor type
    /// is not dispatched, the handler returned false (no such descriptor),
    /// or `out` is too small to hold the full wire_size(). The caller
    /// should treat any 0 return as AEM_STATUS_NO_SUCH_DESCRIPTOR.
    [[nodiscard]] auto get_descriptor_for_wire(DescriptorRef ref, std::span<uint8_t> out) const -> size_t;

    /// Populate `out` with a GET_NAME response body (IEEE 1722.1
    /// Clause 7.4.18.1): 8-byte header plus a 64-byte AtdeccString.
    /// Returns 72 on success, 0 if the handler has no name for this
    /// (ref) or `out` is too small. A 0 return should map to
    /// AEM_STATUS_NO_SUCH_DESCRIPTOR.
    [[nodiscard]] auto get_name_for_wire(NameRef ref, std::span<uint8_t> out) const -> size_t;

    /// Apply a SET_NAME command (IEEE 1722.1 Clause 7.4.17).
    /// `command_body` is the 72-byte payload following the AemDu header:
    /// descriptor_type (2) + descriptor_index (2) + name_index (2) +
    /// configuration_index (2) + name (64). Returns an AEM_STATUS_* code
    /// suitable for the response.
    [[nodiscard]] auto apply_set_name(std::span<uint8_t const> command_body) -> uint8_t;

    /// Result of an AEM SET descriptor-value command: the AEM status + the number of
    /// response-body bytes written (the echoed command).
    struct SetValueResult
    {
        uint8_t status{};
        size_t size{};
    };

    /// Apply an AEM SET descriptor-value command (the family: SET_CONTROL,
    /// SET_STREAM_FORMAT, SET_SAMPLING_RATE, ...). @p command_body is
    /// descriptor_type(2) + descriptor_index(2) + value; @p command_type is the
    /// AEM_COMMAND_SET_* code. Resolves the descriptor's well-known symbol, dispatches
    /// to handler.on_set_descriptor_value, and echoes the command into @p out as the
    /// response body (AECP echoes the SET command).
    [[nodiscard]] auto apply_set_descriptor_value(
        uint16_t command_type, std::span<uint8_t const> command_body, std::span<uint8_t> out) -> SetValueResult;

    /// Frame an AEM GET descriptor-value response (GET_CONTROL, GET_STREAM_FORMAT, ...).
    /// @p command_body is descriptor_type(2) + descriptor_index(2). Writes type/index +
    /// the handler's current value (on_get_descriptor_value) into @p out; returns the
    /// response size, or 0 if no such descriptor. @p command_type is the AEM_COMMAND_GET_* code.
    [[nodiscard]] auto get_descriptor_value_for_wire(
        uint16_t command_type, std::span<uint8_t const> command_body, std::span<uint8_t> out) const -> size_t;

    /// True if a DescriptorStorage blob is attached.
    [[nodiscard]] auto has_static_store() const noexcept -> bool { return storage_.has_value(); }

    /// Raw pointer to the attached DescriptorStorage, or nullptr if none.
    [[nodiscard]] auto static_store() const noexcept -> DescriptorStorage const* { return storage_ ? &*storage_ : nullptr; }

    /// Access the underlying handler. Useful for tests and out-of-band
    /// calls that don't go through the wire-format path.
    [[nodiscard]] auto handler() const noexcept -> AemEntityHandler& { return *handler_; }

  private:
    /// Look up the pre-agreed symbol for a (configuration, type, index)
    /// triple via the attached DescriptorStorage symbol table, returning
    /// 0 if no storage is attached or no symbol is registered.
    [[nodiscard]] auto symbol_for(DescriptorRef ref) const -> uint32_t;

    AemEntityHandler* handler_{nullptr};
    std::optional<DescriptorStorage> storage_;
    uint16_t configuration_{0};  ///< see set_configuration
};

}  // namespace statusbar::nanoavb
