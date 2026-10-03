-- SPDX-License-Identifier: MIT
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
local AECP_AVC_COMMAND = 4
local AECP_AVC_RESPONSE = 5
local AECP_HDCP_APM_COMMAND = 8
local AECP_HDCP_APM_RESPONSE = 9
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

-- Control value types seen in descriptors, so SET/GET_CONTROL (and MIXER /
-- MATRIX / SIGNAL_TRANSCODER value payloads, which carry no type of their
-- own) can be decoded: "<target entity>:<descriptor_type>:<descriptor_index>"
-- -> list of { value_type, count, frame }. Learned on the first pass from
-- READ_DESCRIPTOR responses; a lookup only uses what an EARLIER frame taught,
-- so a frame dissects the same way on every pass and a SET_CONTROL seen
-- before its descriptor stays raw.
local control_types = {}

local function learn_control_type(pinfo, key, value_type, count)
    if pinfo.visited then
        return
    end
    local list = control_types[key]
    if list == nil then
        list = {}
        control_types[key] = list
    end
    list[#list + 1] = { value_type = value_type, count = count, frame = pinfo.number }
end

local function known_control_type(pinfo, key)
    local list = control_types[key]
    if list == nil then
        return nil
    end
    local found = nil
    for _, entry in ipairs(list) do
        if entry.frame < pinfo.number then
            found = entry
        end
    end
    return found
end

local STREAM_FORMAT_NAMES = { [0x00] = "IEC 61883-6", [0x02] = "AAF", [0x04] = "CRF" }

--- The sub-fields of an 8-octet stream format at `at` (AAF, IEC 61883-6, CRF)
local function add_stream_format(t, tvb, at)
    local subtype = tvb(at, 1):uint()
    local add = gen.stream_format_add[subtype]
    if add == nil then
        return
    end
    local sf = t:add(tvb(at, 8), string.format("stream format: %s 0x%s", STREAM_FORMAT_NAMES[subtype], tostring(tvb(at, 8):bytes()):lower()))
    add(sf, tvb, at)
end

local function add_units(t, tvb, at)
    local code = tvb(at + 1, 1):uint()
    local u = t:add(tvb(at, 2), string.format("units: %s x 10^%d", gen.values_control_unit_code[code] or string.format("0x%02x", code),
        tvb(at, 1):int()))
    u:add(gen.f.atdecc_control_units_multiplier, tvb(at, 1))
    u:add(gen.f.atdecc_control_units_code, tvb(at + 1, 1))
end

--- Control value_details at [at, stop) for a 14-bit control_value_type and
--- `n` values (IEEE 1722.1-2021 7.3.5.2, Table 7.12). mode "full" is a
--- descriptor's value_details (ranges, units, strings and current values);
--- mode "current" is the current-value-only form of SET/GET_CONTROL and the
--- MIXER/MATRIX/TRANSCODER payloads. `raw_field` shows what is left over.
--- Returns the offset after the values.
local function add_control_values(t, tvb, at, stop, value_type, n, mode, raw_field)
    local fam = gen.control_value_family[value_type]
    local name = gen.values_control_value_type[value_type] or string.format("value_type 0x%04x", value_type)
    if fam == nil or fam.family == "vendor" or fam.family == "expansion" then
        if stop > at then
            t:add(gen.CONTROL_VENDOR, tvb(at, stop - at)):append_text(" [" .. name .. "]")
        end
        return stop
    end
    local V = fam.size
    local function typed(parent, pos, role)
        local item = parent:add(fam.field, tvb(pos, V))
        item:append_text(" (" .. role .. ")")
        return item
    end
    if fam.family == "linear" then
        if mode == "current" then
            for i = 0, n - 1 do
                if at + V > stop then break end
                typed(t, at, "current[" .. i .. "]")
                at = at + V
            end
        else
            local entry = 5 * V + 4
            for i = 0, n - 1 do
                if at + entry > stop then
                    t:add_proto_expert_info(ef_trailer)
                    break
                end
                local e = t:add(tvb(at, entry), string.format("%s value[%d]", name, i))
                typed(e, at, "minimum")
                typed(e, at + V, "maximum")
                typed(e, at + 2 * V, "step")
                typed(e, at + 3 * V, "default")
                typed(e, at + 4 * V, "current")
                add_units(e, tvb, at + 5 * V)
                e:add(gen.f.atdecc_control_localized_string, tvb(at + 5 * V + 2, 2))
                at = at + entry
            end
        end
    elseif fam.family == "selector" then
        if mode == "current" then
            if at + V <= stop then
                typed(t, at, "current")
                at = at + V
            end
        else
            local total = (n + 2) * V + 2
            if at + total > stop then
                t:add_proto_expert_info(ef_trailer)
            else
                local e = t:add(tvb(at, total), string.format("%s %d options", name, n))
                typed(e, at, "current")
                typed(e, at + V, "default")
                for i = 0, n - 1 do
                    typed(e, at + (2 + i) * V, "option[" .. i .. "]")
                end
                add_units(e, tvb, at + (n + 2) * V)
                at = at + total
            end
        end
    elseif fam.family == "array" then
        if mode == "current" then
            for i = 0, n - 1 do
                if at + V > stop then break end
                typed(t, at, "current[" .. i .. "]")
                at = at + V
            end
        else
            local total = (n + 4) * V + 4
            if at + total > stop then
                t:add_proto_expert_info(ef_trailer)
            else
                local e = t:add(tvb(at, total), string.format("%s %d values", name, n))
                typed(e, at, "minimum")
                typed(e, at + V, "maximum")
                typed(e, at + 2 * V, "step")
                typed(e, at + 3 * V, "default")
                add_units(e, tvb, at + 4 * V)
                e:add(gen.f.atdecc_control_localized_string, tvb(at + 4 * V + 2, 2))
                for i = 0, n - 1 do
                    typed(e, at + 4 * V + 4 + i * V, "current[" .. i .. "]")
                end
                at = at + total
            end
        end
    elseif fam.family == "utf8" then
        if stop > at then
            t:add(gen.CONTROL_UTF8, tvb(at, stop - at))
        end
        at = stop
    elseif fam.family == "bode_plot" then
        if mode ~= "current" then
            if at + 48 > stop then
                t:add_proto_expert_info(ef_trailer)
                return at
            end
            gen.add_control_bode(t:add(tvb(at, 48), "bode plot ranges"), tvb, at)
            at = at + 48
        end
        for i = 0, n - 1 do
            if at + 12 > stop then break end
            gen.add_control_bode_point(t:add(tvb(at, 12), string.format("point[%d]", i)), tvb, at)
            at = at + 12
        end
    elseif fam.family == "smpte_time" then
        if at + 10 <= stop then
            gen.add_control_smpte(t, tvb, at)
            at = at + 10
        end
    elseif fam.family == "sample_rate" then
        if at + 16 <= stop then
            gen.add_control_sample_rate(t, tvb, at)
            at = at + 16
        elseif at + 4 <= stop then
            t:add(gen.f.atdecc_control_sample_rate_current_pull, tvb(at, 4))
            t:add(gen.f.atdecc_control_sample_rate_current_base_frequency, tvb(at, 4))
            at = at + 4
        end
    elseif fam.family == "gptp_time" then
        if at + 10 <= stop then
            gen.add_control_gptp(t, tvb, at)
            at = at + 10
        end
    end
    if raw_field ~= nil and stop > at then
        t:add(raw_field, tvb(at, stop - at))
        at = stop
    end
    return at
end

--- A descriptor at `at` (its descriptor_type leads) belonging to `target`;
--- returns its name
local function dissect_descriptor(t, tvb, at, stop, target, pinfo)
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
    for _, off in ipairs(gen.stream_format_at[spec.name] or {}) do
        add_stream_format(sub, tvb, at + off)
    end
    for _, tab in ipairs(spec.tables) do
        local count = tab.count_field_offset and tvb(at + tab.count_field_offset, 2):uint() or 1
        local offset = tvb(at + tab.offset_field_offset, 2):uint()
        local start = at + offset
        if tab.element == "values" then
            local value_type = tvb(at + tab.value_type_offset, 2):uint() % 0x4000
            if target ~= nil then
                learn_control_type(pinfo, string.format("%s:%d:%d", target, kind, tvb(at + 2, 2):uint()), value_type, count)
            end
            if start < stop then
                add_control_values(sub, tvb, start, stop, value_type, count, "full", nil)
            end
        elseif start + (count * tab.element_size) > stop then
            sub:add_proto_expert_info(ef_trailer)
        else
            for i = 0, count - 1 do
                local pos = start + (i * tab.element_size)
                local element = sub:add(tvb(pos, tab.element_size), string.format("%s[%d]", tab.element, i))
                tab.element_add(element, tvb, pos)
                if tab.element == "stream_format" then
                    add_stream_format(element, tvb, pos)
                end
            end
        end
    end
    return name
end

--- The SET_CONTROL/GET_CONTROL values when the control's type is unknown:
--- raw, or a JDKS blob when it carries one
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

local DESCRIPTOR_MATRIX = 0x0010

--- The values after a CONTROL / MIXER / MATRIX / SIGNAL_TRANSCODER payload
--- header (descriptor_type and index lead the payload), typed from the
--- descriptor seen earlier when possible
local function dissect_payload_values(t, tvb, at, stop, target, spec, pinfo)
    local kind = tvb(gen.AEM_HEADER_LENGTH, 2):uint()
    local index = tvb(gen.AEM_HEADER_LENGTH + 2, 2):uint()
    local known = known_control_type(pinfo, string.format("%s:%d:%d", target, kind, index))
    if known ~= nil and stop > at then
        local n = known.count
        if kind == DESCRIPTOR_MATRIX then
            n = tvb(gen.AEM_HEADER_LENGTH + 12, 2):uint() % 0x4000  -- rep_direction_value_count
        elseif spec.name:find("mixer") then
            n = 1
        end
        add_control_values(t, tvb, at, stop, known.value_type, n, "current", gen.AEM_PAYLOAD_RAW)
        return
    end
    dissect_control_values(t, tvb, at, stop)
end

--- GET_DYNAMIC_INFO (7.4.76): dynamic_info entries from `at`, each an
--- 8-octet header and the AEM payload of its command (fixed-size GETs only,
--- so the command's own payload spec applies with no trailer)
local function dissect_dynamic_infos(t, tvb, at, stop, is_command)
    local index = 0
    while at + gen.DYNAMIC_INFO_HEADER_LENGTH <= stop do
        local length = tvb(at, 2):uint()
        local code = tvb(at + 6, 2):uint() % 0x4000
        local total = gen.DYNAMIC_INFO_HEADER_LENGTH + length
        if at + total > stop then
            t:add_proto_expert_info(ef_trailer)
            total = stop - at
        end
        local entry = t:add(tvb(at, total), string.format("dynamic_info[%d]: %s %s", index,
            gen.values_aem_command[code] or string.format("command 0x%04x", code), is_command and "command" or "response"))
        gen.add_aem_dynamic_info(entry, tvb, at)
        local specs = gen.aem_payloads[code]
        local spec = specs and (is_command and specs.cmd or specs.rsp)
        local body = at + gen.DYNAMIC_INFO_HEADER_LENGTH
        if spec ~= nil and spec.trailer == nil and length >= spec.length then
            -- the payload layouts are AEM-header relative: shift them to this entry
            spec.add(entry, tvb, body - gen.AEM_HEADER_LENGTH)
            if length > spec.length then
                entry:add(gen.AEM_PAYLOAD_RAW, tvb(body + spec.length, length - spec.length))
            end
        elseif length > 0 then
            entry:add(gen.AEM_PAYLOAD_RAW, tvb(body, math.min(length, stop - body)))
        end
        at = at + total
        index = index + 1
    end
    return at
end

--- AEM command/response after the 24-octet AEM header; returns the summary
local function dissect_aem(t, tvb, pinfo, stop, is_command)
    gen.add_aem(t, tvb, 0)
    local code = tvb(22, 2):uint() % 0x4000
    local name = gen.values_aem_command[code] or string.format("command 0x%04x", code)
    local specs = gen.aem_payloads[code]
    local spec = specs and (is_command and specs.cmd or specs.rsp)
    local target = eui64_hex(tvb(4, 8))
    local at = gen.AEM_HEADER_LENGTH
    local extra = ""
    if spec ~= nil and stop - at >= spec.length then
        spec.add(t, tvb, 0)
        for _, off in ipairs(gen.stream_format_at[spec.name] or {}) do
            add_stream_format(t, tvb, off)
        end
        at = at + spec.length
        local trailer = spec.trailer
        if trailer ~= nil then
            if trailer.kind == "descriptor" then
                if stop - at >= 4 then
                    extra = " " .. dissect_descriptor(t, tvb, at, stop, target, pinfo)
                end
                at = stop
            elseif trailer.kind == "values" or trailer.kind == "raw" then
                dissect_payload_values(t, tvb, at, stop, target, spec, pinfo)
                at = stop
            elseif trailer.kind == "elements" then
                local count = tvb(trailer.count_field_offset, 2):uint()
                for i = 0, count - 1 do
                    if at + trailer.element_size > stop then
                        t:add_proto_expert_info(ef_trailer)
                        break
                    end
                    local element = t:add(tvb(at, trailer.element_size), string.format("%s[%d]", trailer.element, i))
                    trailer.element_add(element, tvb, at)
                    at = at + trailer.element_size
                end
            elseif trailer.kind == "blob" then
                local length = math.min(tvb(trailer.count_field_offset, 2):uint(), stop - at)
                if length > 0 then
                    t:add(trailer.field, tvb(at, length))
                    at = at + length
                end
            elseif trailer.kind == "dynamic_infos" then
                at = dissect_dynamic_infos(t, tvb, at, stop, is_command)
            end
        end
    end
    if stop > at then
        t:add(gen.AEM_PAYLOAD_RAW, tvb(at, stop - at))
    end
    return name .. extra
end

--- Milan vendor unique command/response after the 28-octet VU header
local function dissect_mvu(t, tvb, stop, is_command)
    gen.add_mvu(t, tvb, 0)
    local code = tvb(28, 2):uint() % 0x8000
    local name = gen.values_mvu_command[code] or string.format("command 0x%04x", code)
    local specs = gen.mvu_payloads[code]
    local spec = specs and (is_command and specs.cmd or specs.rsp)
    local at = gen.MVU_HEADER_LENGTH
    if spec ~= nil and stop - at >= spec.length then
        spec.add(t, tvb, 0)
        at = at + spec.length
        if spec.name == "mvu_get_milan_info_response" then
            for _, f in ipairs(gen.mvu_features_flag_fields) do
                t:add(f, tvb(gen.MVU_HEADER_LENGTH + 6, 4))
            end
        elseif spec.name == "mvu_media_clock_reference_info" then
            for _, f in ipairs(gen.mvu_mcr_flag_fields) do
                t:add(f, tvb(gen.MVU_HEADER_LENGTH + 2, 1))
            end
        end
        if spec.name_field ~= nil and stop - at >= 64 then
            t:add(spec.name_field, tvb(at, 64))
            at = at + 64
        end
    end
    if stop > at then
        t:add(gen.VU_PAYLOAD, tvb(at, stop - at))
    end
    return "MVU " .. name
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
    local status_code = tvb(2, 2):bitfield(0, 5)
    local status = gen.values_aem_status[status_code] or tostring(status_code)
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
    elseif (msg_type == AECP_AVC_COMMAND or msg_type == AECP_AVC_RESPONSE) and len >= gen.AVC_HEADER_LENGTH then
        gen.add_avc(t, tvb, 0)
        local length = math.min(tvb(22, 2):uint(), stop - gen.AVC_HEADER_LENGTH)
        if length > 0 then
            t:add(gen.AVC_PAYLOAD, tvb(gen.AVC_HEADER_LENGTH, length))
        end
        summary = string.format("AVC %s %d octets", is_command and "command" or "response", length)
    elseif (msg_type == AECP_HDCP_APM_COMMAND or msg_type == AECP_HDCP_APM_RESPONSE) and len >= gen.HDCP_APM_HEADER_LENGTH then
        gen.add_hdcp_apm(t, tvb, 0)
        if stop > gen.HDCP_APM_HEADER_LENGTH then
            t:add(gen.HDCP_APM_DATA, tvb(gen.HDCP_APM_HEADER_LENGTH, stop - gen.HDCP_APM_HEADER_LENGTH))
        end
        summary = string.format("HDCP APM %s offset=%d%s", is_command and "command" or "response", tvb(26, 2):uint(),
            tvb(24, 1):bitfield(7, 1) == 1 and " more fragments" or "")
    elseif (msg_type == AECP_VU_COMMAND or msg_type == AECP_VU_RESPONSE) and len >= gen.VU_HEADER_LENGTH then
        gen.add_vu(t, tvb, 0)
        local protocol_id = tostring(tvb(22, 6):bytes()):lower()
        if protocol_id == gen.MVU_PROTOCOL_ID and len >= gen.MVU_HEADER_LENGTH then
            summary = string.format("%s %s", dissect_mvu(t, tvb, stop, is_command), is_command and "command" or "response")
            status = gen.values_mvu_status[status_code] or status
        else
            if stop > gen.VU_HEADER_LENGTH then
                t:add(gen.VU_PAYLOAD, tvb(gen.VU_HEADER_LENGTH, stop - gen.VU_HEADER_LENGTH))
            end
            summary = string.format("Vendor Unique %s protocol_id=%s", is_command and "command" or "response", protocol_id)
        end
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
