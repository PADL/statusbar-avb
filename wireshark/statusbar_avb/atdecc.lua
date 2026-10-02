-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- IEEE 1722.1-2021 ATDECC: ADP (subtype 0xFA), AECP (0xFB) and ACMP (0xFC).
-- AECP covers AEM commands and responses with every payload the schema
-- describes, READ_DESCRIPTOR responses down to each descriptor's counted
-- trailer, Address Access TLVs, Vendor Unique with the JDKS log and IPv4
-- blobs carried in SET_CONTROL values, and command/response pairing for
-- AECP and ACMP. Field tables and layout decoders come from the generated
-- gen/atdecc_fields.lua (python/wireshark_schema/atdecc_table.py).
--
-- Written from IEEE Std 1722.1-2021 and this repository's atdecc_*.hpp wire
-- definitions; nothing here derives from Wireshark's own dissectors.

-- Wireshark 4.x executes every .lua file it finds under a plugin folder,
-- including this module, passing it (basename, path); only a require() from
-- the loader statusbar_avb.lua, which passes the dotted module name, may run it.
if type((...)) ~= "string" or not (...):find("^statusbar_avb%.") then
    return
end

local M = {}

local gen = require("statusbar_avb.gen.atdecc_fields")

local ADP = 0xFA
local AECP = 0xFB
local ACMP = 0xFC
local ADP_LENGTH = 68
local ACMP_LENGTH = 56

local AECP_AEM_COMMAND = 0
local AECP_AEM_RESPONSE = 1
local AECP_AA_COMMAND = 2
local AECP_AA_RESPONSE = 3
local AECP_VU_COMMAND = 6
local AECP_VU_RESPONSE = 7
local AEM_READ_DESCRIPTOR = 0x0004

M.proto = Proto.new("avb.atdecc", "IEEE 1722.1 ATDECC (statusbar)")

local f_response_in = ProtoField.framenum("avb.atdecc.response_in", "Response in", base.NONE, frametype.RESPONSE)
local f_response_to = ProtoField.framenum("avb.atdecc.response_to", "Response to", base.NONE, frametype.REQUEST)
local f_response_time = ProtoField.relative_time("avb.atdecc.response_time", "Response time")
local fields = {}
for _, f in ipairs(gen.fields) do
    fields[#fields + 1] = f
end
fields[#fields + 1] = f_response_in
fields[#fields + 1] = f_response_to
fields[#fields + 1] = f_response_time
M.proto.fields = fields

local ef_short = ProtoExpert.new("avb.atdecc.expert.short", "ATDECC PDU shorter than its header",
    expert.group.MALFORMED, expert.severity.ERROR)
local ef_cdl = ProtoExpert.new("avb.atdecc.expert.control_data_length",
    "control_data_length runs past the captured PDU", expert.group.MALFORMED, expert.severity.WARN)
local ef_unknown_descriptor = ProtoExpert.new("avb.atdecc.expert.unknown_descriptor",
    "READ_DESCRIPTOR carries an unknown descriptor type", expert.group.UNDECODED, expert.severity.NOTE)
local ef_trailer = ProtoExpert.new("avb.atdecc.expert.trailer", "descriptor trailer runs past the descriptor",
    expert.group.MALFORMED, expert.severity.WARN)
local ef_no_command = ProtoExpert.new("avb.atdecc.expert.unmatched_response",
    "response without a matching command in this capture", expert.group.SEQUENCE, expert.severity.NOTE)
M.proto.experts = { ef_short, ef_cdl, ef_unknown_descriptor, ef_trailer, ef_no_command }

-- Command/response pairing (two-pass): commands[key] = frame of the last
-- command seen with that key; responses[frame] = { frame, time } of the
-- command a response answers; response_in[frame] = the answering frame.
local commands = {}
local responses = {}
local response_in = {}

local function eui64_hex(range)
    return "0x" .. tostring(range:bytes()):lower()
end

local function pair(pinfo, tree, key, is_command)
    if not pinfo.visited then
        if is_command then
            commands[key] = { frame = pinfo.number, time = pinfo.abs_ts }
        else
            local command = commands[key]
            if command ~= nil then
                responses[pinfo.number] = command
                response_in[command.frame] = pinfo.number
            end
        end
    end
    if is_command then
        local answer = response_in[pinfo.number]
        if answer ~= nil then
            tree:add(f_response_in, answer):set_generated()
        end
    else
        local command = responses[pinfo.number]
        if command ~= nil then
            tree:add(f_response_to, command.frame):set_generated()
            local delta = pinfo.abs_ts - command.time
            local secs = math.floor(delta)
            tree:add(f_response_time, NSTime.new(secs, math.floor((delta - secs) * 1e9 + 0.5))):set_generated()
        else
            tree:add_proto_expert_info(ef_no_command)
        end
    end
end

-- ADP ------------------------------------------------------------------------

local function dissect_adp(tvb, pinfo, root, header, tree)
    local t = tree:add(M.proto, tvb(), "IEEE 1722.1 ADP")
    if tvb:len() < ADP_LENGTH then
        t:add_proto_expert_info(ef_short)
        return
    end
    gen.add_adp(t, tvb, 0)
    local kind = gen.values_adp_message_type[tvb(1, 1):bitfield(4, 4)] or "ADP"
    pinfo.cols.protocol = "ADP"
    pinfo.cols.info:set(string.format("%s entity_id=%s model=%s available_index=%d", kind, eui64_hex(tvb(4, 8)),
        eui64_hex(tvb(12, 8)), tvb(36, 4):uint()))
end

-- ACMP -----------------------------------------------------------------------

local function dissect_acmp(tvb, pinfo, root, header, tree)
    local t = tree:add(M.proto, tvb(), "IEEE 1722.1 ACMP")
    if tvb:len() < ACMP_LENGTH then
        t:add_proto_expert_info(ef_short)
        return
    end
    gen.add_acmp(t, tvb, 0)
    local msg_type = tvb(1, 1):bitfield(4, 4)
    local kind = gen.values_acmp_message_type[msg_type] or string.format("message_type %d", msg_type)
    local status = gen.values_acmp_status[tvb(2, 2):bitfield(0, 5)] or "?"
    local seq = tvb(48, 2):uint()
    pinfo.cols.protocol = "ACMP"
    pinfo.cols.info:set(string.format("%s talker=%s:%d listener=%s:%d seq=%d%s", kind, eui64_hex(tvb(20, 8)),
        tvb(36, 2):uint(), eui64_hex(tvb(28, 8)), tvb(38, 2):uint(), seq,
        msg_type % 2 == 1 and (" " .. status) or ""))
    -- Responses share the command's controller id, sequence id and message
    -- type with the low bit set; pair on the even message type.
    local key = string.format("acmp:%s:%d:%d", eui64_hex(tvb(12, 8)), seq, msg_type - (msg_type % 2))
    pair(pinfo, t, key, msg_type % 2 == 0)
end

-- AECP -----------------------------------------------------------------------

--- A descriptor at `at` (its descriptor_type leads); returns its name
local function dissect_descriptor(t, tvb, at, stop)
    local kind = tvb(at, 2):uint()
    local spec = gen.descriptors[kind]
    local name = gen.values_descriptor_type[kind] or string.format("descriptor 0x%04x", kind)
    local sub = t:add(tvb(at, stop - at), "Descriptor " .. name)
    if spec == nil then
        sub:add_proto_expert_info(ef_unknown_descriptor)
        return name
    end
    if stop - at < spec.length then
        sub:add_proto_expert_info(ef_short)
        return name
    end
    spec.add(sub, tvb, at)
    local trailer = spec.trailer
    if trailer ~= nil then
        local count = tvb(at + trailer.count_field_offset, 2):uint()
        local offset = tvb(at + trailer.offset_field_offset, 2):uint()
        if trailer.element == "raw" then
            if at + offset < stop then
                sub:add(gen.DESCRIPTOR_RAW, tvb(at + offset, stop - at - offset))
            end
        else
            local start = at + offset
            if start + (count * trailer.element_size) > stop then
                sub:add_proto_expert_info(ef_trailer)
                count = math.max(0, math.floor((stop - start) / trailer.element_size))
            end
            for i = 0, count - 1 do
                local element = sub:add(tvb(start + (i * trailer.element_size), trailer.element_size),
                    string.format("%s[%d]", trailer.element, i))
                trailer.element_add(element, tvb, start + (i * trailer.element_size))
            end
        end
    end
    return name
end

--- The SET_CONTROL/GET_CONTROL values: raw, or a JDKS blob when it carries one
local function dissect_control_values(t, tvb, at, stop)
    if stop - at >= gen.JDKS_LOG_BLOB_LENGTH then
        local vendor = tostring(tvb(at, 8):bytes()):lower()
        if vendor == gen.JDKS_CONTROL_LOG_TEXT then
            local blob = t:add(tvb(at, stop - at), "JDKS log message")
            gen.add_jdks_log(blob, tvb, at)
            local text_length = math.min(math.max(tvb(at + 8, 4):uint() - 2, 0), stop - at - gen.JDKS_LOG_BLOB_LENGTH)
            if text_length > 0 then
                blob:add(gen.JDKS_LOG_TEXT, tvb(at + gen.JDKS_LOG_BLOB_LENGTH, text_length))
            end
            return
        elseif vendor == gen.JDKS_CONTROL_IPV4_PARAMETERS and stop - at >= 8 + gen.JDKS_IPV4_PARAMS_LENGTH then
            local blob = t:add(tvb(at, stop - at), "JDKS IPv4 parameters")
            gen.add_jdks_ipv4(blob, tvb, at + 8)
            return
        end
    end
    if stop > at then
        t:add(gen.AEM_PAYLOAD_RAW, tvb(at, stop - at))
    end
end

--- AEM command/response after the 24-octet AEM header; returns the summary
local function dissect_aem(t, tvb, pinfo, stop, is_command)
    gen.add_aem(t, tvb, 0)
    local code = tvb(22, 2):uint() % 0x4000
    local name = gen.values_aem_command[code] or string.format("command 0x%04x", code)
    local specs = gen.aem_payloads[code]
    local spec = specs and (is_command and specs.cmd or specs.rsp)
    local at = gen.AEM_HEADER_LENGTH
    local extra = ""
    if spec ~= nil and stop - at >= spec.length then
        spec.add(t, tvb, 0)
        at = at + spec.length
        if spec.trailer == "descriptor" then
            if stop - at >= 4 then
                extra = " " .. dissect_descriptor(t, tvb, at, stop)
            end
            at = stop
        elseif spec.trailer == "values" then
            dissect_control_values(t, tvb, at, stop)
            at = stop
        elseif spec.trailer == "mappings" then
            local count_at = spec.name == "audio_map_response" and (gen.AEM_HEADER_LENGTH + 6) or (gen.AEM_HEADER_LENGTH + 4)
            local count = tvb(count_at, 2):uint()
            for i = 0, count - 1 do
                if at + gen.AUDIO_MAPPING_LENGTH > stop then
                    break
                end
                local element = t:add(tvb(at, gen.AUDIO_MAPPING_LENGTH), string.format("mapping[%d]", i))
                gen.add_aem_audio_mapping(element, tvb, at)
                at = at + gen.AUDIO_MAPPING_LENGTH
            end
        end
    end
    if stop > at then
        t:add(gen.AEM_PAYLOAD_RAW, tvb(at, stop - at))
    end
    return name .. extra
end

local function dissect_aecp(tvb, pinfo, root, header, tree)
    local len = tvb:len()
    local t = tree:add(M.proto, tvb(), "IEEE 1722.1 AECP")
    if len < gen.AECP_HEADER_LENGTH then
        t:add_proto_expert_info(ef_short)
        return
    end
    gen.add_aecp(t, tvb, 0)
    local msg_type = tvb(1, 1):bitfield(4, 4)
    local cdl = tvb(2, 2):uint() % 0x800
    local stop = 12 + cdl
    if stop > len then
        t:add_proto_expert_info(ef_cdl)
        stop = len
    end
    local kind = gen.values_aecp_message_type[msg_type] or string.format("message_type %d", msg_type)
    local status = gen.values_aem_status[tvb(2, 2):bitfield(0, 5)] or tostring(tvb(2, 2):bitfield(0, 5))
    local seq = tvb(20, 2):uint()
    local summary = kind
    local is_command = msg_type % 2 == 0
    pinfo.cols.protocol = "AECP"

    if (msg_type == AECP_AEM_COMMAND or msg_type == AECP_AEM_RESPONSE) and len >= gen.AEM_HEADER_LENGTH then
        summary = string.format("AEM %s %s", dissect_aem(t, tvb, pinfo, stop, is_command), is_command and "command" or "response")
    elseif (msg_type == AECP_AA_COMMAND or msg_type == AECP_AA_RESPONSE) and len >= gen.AA_HEADER_LENGTH then
        gen.add_aa(t, tvb, 0)
        local at = gen.AA_HEADER_LENGTH
        local count = 0
        while at + 10 <= stop do
            local length = tvb(at, 2):uint() % 0x1000
            local tlv = t:add(tvb(at, math.min(10 + length, stop - at)), string.format("TLV[%d]", count))
            gen.add_aa_tlv(tlv, tvb, at)
            if length > 0 and at + 10 + length <= stop then
                tlv:add(gen.AA_TLV_DATA, tvb(at + 10, length))
            end
            at = at + 10 + length
            count = count + 1
        end
        summary = string.format("Address Access %s %d TLV%s", is_command and "command" or "response", count, count == 1 and "" or "s")
    elseif (msg_type == AECP_VU_COMMAND or msg_type == AECP_VU_RESPONSE) and len >= gen.VU_HEADER_LENGTH then
        gen.add_vu(t, tvb, 0)
        if stop > gen.VU_HEADER_LENGTH then
            t:add(gen.VU_PAYLOAD, tvb(gen.VU_HEADER_LENGTH, stop - gen.VU_HEADER_LENGTH))
        end
        summary = string.format("Vendor Unique %s protocol_id=%s", is_command and "command" or "response",
            tostring(tvb(22, 6):bytes()):lower())
    elseif stop > gen.AECP_HEADER_LENGTH then
        t:add(gen.AECP_PAYLOAD, tvb(gen.AECP_HEADER_LENGTH, stop - gen.AECP_HEADER_LENGTH))
    end
    pinfo.cols.info:set(string.format("%s seq=%d target=%s%s", summary, seq, eui64_hex(tvb(4, 8)),
        is_command and "" or (" " .. status)))
    local key = string.format("aecp:%s:%s:%d", eui64_hex(tvb(12, 8)), eui64_hex(tvb(4, 8)), seq)
    pair(pinfo, t, key, is_command)
end

function M.register(avtp)
    avtp.subtype_dissectors[ADP] = dissect_adp
    avtp.subtype_dissectors[AECP] = dissect_aecp
    avtp.subtype_dissectors[ACMP] = dissect_acmp
end

return M
