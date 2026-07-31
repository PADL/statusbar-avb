// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_jdks.hpp"

#include <string_view>

namespace statusbar::atdecc::jdks {

auto log_priority_name(uint8_t const priority) noexcept -> std::string_view
{
    switch (priority) {
        case log_priority::ERROR:
            return "Error";
        case log_priority::WARNING:
            return "Warning";
        case log_priority::INFO:
            return "Info";
        case log_priority::DEBUG1:
            return "Debug1";
        case log_priority::DEBUG2:
            return "Debug2";
        case log_priority::DEBUG3:
            return "Debug3";
        case log_priority::CONSOLE:
            return "Console";
        default:
            return "Unknown";
    }
}

auto generate_log_response(
    std::span<uint8_t> buffer, ConsoleCommandContext const& ctx, uint16_t& sequence_id, uint8_t log_detail, std::string_view text)
    -> size_t
{
    using ieee::EthernetFrame;
    using statusbar::BufferSerializerBuilder;
    using statusbar::MutableBuffer;

    // Validate text length
    size_t const text_len = std::min(text.size(), LOG_MAX_TEXT_LEN);

    // Calculate frame size
    constexpr size_t ETH_HEADER_LEN = 14;  // Untagged ethernet frame
    size_t const frame_len =
        ETH_HEADER_LEN + AemDu::LENGTH + aem::AemControlPayloadHeader::LENGTH + LogBlobHeader::LENGTH + text_len;

    if (buffer.size() < frame_len) {
        return 0;
    }

    // Build Ethernet frame header (always multicast for log responses)
    EthernetFrame eth{};
    eth.dest_mac = MULTICAST_LOG;
    eth.src_mac = ctx.src_mac;
    eth.ethertype = avtp::AVTP_ETHERTYPE;

    // Build AEM header
    AemDu aem{};
    aem.subtype = AvtpSubtype::aecp;
    aem.set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
    aem.set_status(AEM_STATUS_SUCCESS);
    // control_data_length = AEMDU fields after common header + control header + blob
    uint16_t const cdl =
        static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + aem::AemControlPayloadHeader::LENGTH + LogBlobHeader::LENGTH + text_len);
    aem.set_control_data_length(cdl);
    aem.target_entity_id = ctx.my_entity_id;
    aem.controller_entity_id = NOTIFICATIONS_CONTROLLER_ENTITY_ID;
    aem.sequence_id = sequence_id;
    aem.set_unsolicited(true);
    aem.set_command_code(AEM_COMMAND_SET_CONTROL);

    // Build SET_CONTROL header
    aem::AemControlPayloadHeader ctrl_hdr{};
    ctrl_hdr.descriptor_type = aem::DESCRIPTOR_CONTROL;
    ctrl_hdr.descriptor_index = ctx.descriptor_index;

    // Build Log blob header
    LogBlobHeader blob{};
    blob.vendor_eui64 = CONTROL_LOG_TEXT;
    blob.blob_size = static_cast<uint32_t>(text_len + 2);  // text + log_detail + reserved
    blob.log_detail = log_detail;
    blob.reserved = 0;

    // Serialize using BufferSerializerBuilder
    MutableBuffer mut_buf(buffer);
    BufferSerializerBuilder builder(mut_buf);
    builder.append(eth).append(aem).append(ctrl_hdr).append(blob);

    // Append text bytes
    if (text_len > 0) {
        mut_buf.append_unchecked(make_const_span(text).first(text_len));
    }

    if (!builder) {
        return 0;
    }

    // Increment sequence ID on success
    ++sequence_id;

    return mut_buf.size();
}

auto generate_console_command(
    std::span<uint8_t> buffer, ConsoleCommandContext const& ctx, uint16_t& sequence_id, uint8_t log_detail, std::string_view text)
    -> size_t
{
    using ieee::EthernetFrame;
    using statusbar::BufferSerializerBuilder;
    using statusbar::MutableBuffer;

    // Validate text length
    size_t const text_len = std::min(text.size(), LOG_MAX_TEXT_LEN);

    // Calculate frame size
    constexpr size_t ETH_HEADER_LEN = 14;  // Untagged ethernet frame
    size_t const frame_len =
        ETH_HEADER_LEN + AemDu::LENGTH + aem::AemControlPayloadHeader::LENGTH + LogBlobHeader::LENGTH + text_len;

    if (buffer.size() < frame_len) {
        return 0;
    }

    // Build Ethernet frame header
    EthernetFrame eth{};
    eth.dest_mac = ctx.dest_mac;
    eth.src_mac = ctx.src_mac;
    eth.ethertype = avtp::AVTP_ETHERTYPE;

    // Build AEM header
    AemDu aem{};
    aem.subtype = AvtpSubtype::aecp;
    aem.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    aem.set_status(AEM_STATUS_SUCCESS);
    uint16_t const cdl =
        static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + aem::AemControlPayloadHeader::LENGTH + LogBlobHeader::LENGTH + text_len);
    aem.set_control_data_length(cdl);
    aem.target_entity_id = ctx.target_entity_id;
    aem.controller_entity_id = ctx.my_entity_id;
    aem.sequence_id = sequence_id;
    aem.set_unsolicited(false);
    aem.set_command_code(AEM_COMMAND_SET_CONTROL);

    // Build SET_CONTROL header
    aem::AemControlPayloadHeader ctrl_hdr{};
    ctrl_hdr.descriptor_type = aem::DESCRIPTOR_CONTROL;
    ctrl_hdr.descriptor_index = ctx.descriptor_index;

    // Build Log blob header
    LogBlobHeader blob{};
    blob.vendor_eui64 = CONTROL_LOG_TEXT;
    blob.blob_size = static_cast<uint32_t>(text_len + 2);
    blob.log_detail = log_detail;
    blob.reserved = 0;

    // Serialize using BufferSerializerBuilder
    MutableBuffer mut_buf(buffer);
    BufferSerializerBuilder builder(mut_buf);
    builder.append(eth).append(aem).append(ctrl_hdr).append(blob);

    // Append text bytes
    if (text_len > 0) {
        mut_buf.append_unchecked(make_const_span(text).first(text_len));
    }

    if (!builder) {
        return 0;
    }

    // Increment sequence ID on success
    ++sequence_id;

    return mut_buf.size();
}

auto is_log_response(std::span<uint8_t const> data, LogBlobHeader* out_blob) noexcept -> bool
{
    constexpr size_t ETH_HEADER_LEN = 14;
    constexpr size_t AEMDU_LEN = 24;
    constexpr size_t CONTROL_HDR_LEN = 4;
    constexpr size_t BLOB_HDR_LEN = 14;
    constexpr size_t MIN_LEN = ETH_HEADER_LEN + AEMDU_LEN + CONTROL_HDR_LEN + BLOB_HDR_LEN;

    if (data.size() < MIN_LEN) {
        return false;
    }

    // Skip ethernet header
    size_t pos = ETH_HEADER_LEN;

    // Check AVTPDU header
    AemDu aem{};
    span_load(aem, data.subspan(pos));

    // Verify it's an AEM response with SET_CONTROL command
    if (aem.subtype != AvtpSubtype::aecp) {
        return false;
    }
    if (aem.message_type() != AECP_MESSAGE_TYPE_AEM_RESPONSE) {
        return false;
    }
    if (aem.command_code() != AEM_COMMAND_SET_CONTROL) {
        return false;
    }

    pos += AEMDU_LEN;

    // Skip control header
    pos += CONTROL_HDR_LEN;

    // Parse blob header
    LogBlobHeader blob{};
    span_load(blob, data.subspan(pos));

    // Verify vendor EUI-64 matches CONTROL_LOG_TEXT
    if (blob.vendor_eui64 != CONTROL_LOG_TEXT) {
        return false;
    }

    // Verify blob size is reasonable
    uint32_t const blob_size = static_cast<uint32_t>(blob.blob_size);
    if (blob_size < 2 || blob_size > BLOB_MAX_SIZE) {
        return false;
    }

    if (out_blob != nullptr) {
        *out_blob = blob;
    }

    return true;
}

auto is_console_command(std::span<uint8_t const> data, LogBlobHeader* out_blob) noexcept -> bool
{
    constexpr size_t ETH_HEADER_LEN = 14;
    constexpr size_t AEMDU_LEN = 24;
    constexpr size_t CONTROL_HDR_LEN = 4;
    constexpr size_t BLOB_HDR_LEN = 14;
    constexpr size_t MIN_LEN = ETH_HEADER_LEN + AEMDU_LEN + CONTROL_HDR_LEN + BLOB_HDR_LEN;

    if (data.size() < MIN_LEN) {
        return false;
    }

    size_t pos = ETH_HEADER_LEN;

    AemDu aem{};
    span_load(aem, data.subspan(pos));

    if (aem.subtype != AvtpSubtype::aecp) {
        return false;
    }
    if (aem.message_type() != AECP_MESSAGE_TYPE_AEM_COMMAND) {
        return false;
    }
    if (aem.command_code() != AEM_COMMAND_SET_CONTROL) {
        return false;
    }

    pos += AEMDU_LEN + CONTROL_HDR_LEN;

    LogBlobHeader blob{};
    span_load(blob, data.subspan(pos));

    if (blob.vendor_eui64 != CONTROL_LOG_TEXT) {
        return false;
    }

    uint32_t const blob_size = static_cast<uint32_t>(blob.blob_size);
    if (blob_size < 2 || blob_size > BLOB_MAX_SIZE) {
        return false;
    }

    if (out_blob != nullptr) {
        *out_blob = blob;
    }

    return true;
}

auto parse_log_message(std::span<uint8_t const> data, LogMessage& msg) noexcept -> bool
{
    constexpr size_t ETH_HEADER_LEN = 14;
    constexpr size_t AEMDU_LEN = 24;
    constexpr size_t CONTROL_HDR_LEN = 4;
    constexpr size_t BLOB_HDR_LEN = 14;

    LogBlobHeader blob{};
    if (!is_log_response(data, &blob) && !is_console_command(data, &blob)) {
        return false;
    }

    size_t pos = ETH_HEADER_LEN;

    // Parse AEM header
    AemDu aem{};
    span_load(aem, data.subspan(pos));
    pos += AEMDU_LEN;

    // Parse control header
    aem::AemControlPayloadHeader ctrl_hdr{};
    span_load(ctrl_hdr, data.subspan(pos));
    pos += CONTROL_HDR_LEN;

    // Fill in message structure
    msg.target_entity_id = aem.target_entity_id;
    msg.controller_entity_id = aem.controller_entity_id;
    msg.source_entity_id = aem.target_entity_id;  // For responses, target is source
    msg.descriptor_index = static_cast<uint16_t>(ctrl_hdr.descriptor_index);
    msg.sequence_id = static_cast<uint16_t>(aem.sequence_id);
    msg.log_detail = static_cast<uint8_t>(blob.log_detail);

    // Get text
    pos += BLOB_HDR_LEN;
    uint32_t const blob_size = static_cast<uint32_t>(blob.blob_size);
    size_t const text_len = (blob_size >= 2) ? (blob_size - 2) : 0;

    if (text_len > 0 && (pos + text_len) <= data.size()) {
        msg.text = as_string_view(data.subspan(pos, text_len));
    } else {
        msg.text = {};
    }

    return true;
}

}  // namespace statusbar::atdecc::jdks
