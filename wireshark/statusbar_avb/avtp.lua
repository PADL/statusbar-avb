-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- IEEE 1722-2025 AVTP: the common headers (stream version 0 and 1, control,
-- alternative), subtype dispatch, and the Annex J IP encapsulation over
-- UDP. Field definitions and layout decoders come from the generated
-- gen/avtp_fields.lua; per-subtype detail registers into M.subtype_dissectors
-- from the other modules (later waves).
--
-- Written from IEEE Std 1722-2025 and this repository's avtp_*.hpp wire
-- definitions; nothing here derives from Wireshark's own dissectors.

local M = {}

local gen = require("statusbar_avb.gen.avtp_fields")

local AVTP_ETHERTYPE = 0x22F0
local UDP_PORT_CONTINUOUS = 17220
local UDP_PORT_DISCRETE = 17221
local COMMON_HEADER_LENGTH = 12
local STREAM_V0_HEADER_LENGTH = 16
local STREAM_V1_HEADER_LENGTH = 32
local IP_AVTPDU_HEADER_LENGTH = 4

M.proto = Proto.new("avb.avtp", "IEEE 1722 AVTP (statusbar)")
M.ipproto = Proto.new("avb.ipavtp", "IEEE 1722 IP-encapsulated AVTPDU (statusbar)")

M.proto.fields = gen.avtp_fields
M.ipproto.fields = gen.ipavtp_fields

local ef_truncated = ProtoExpert.new("avb.avtp.expert.truncated", "AVTPDU shorter than its header",
    expert.group.MALFORMED, expert.severity.ERROR)
local ef_reserved = ProtoExpert.new("avb.avtp.expert.reserved_subtype", "Reserved AVTP subtype",
    expert.group.UNDECODED, expert.severity.NOTE)
M.proto.experts = { ef_truncated, ef_reserved }

local ef_ip_truncated = ProtoExpert.new("avb.ipavtp.expert.truncated", "IP AVTPDU shorter than its header",
    expert.group.MALFORMED, expert.severity.ERROR)
M.ipproto.experts = { ef_ip_truncated }

-- Per-subtype dissectors: subtype -> function(tvb, pinfo, tree, header)
-- where header carries subtype, version, kind, header_length. A subtype
-- dissector returns nothing; it adds its own subtrees after the common
-- header. Later waves fill this in.
M.subtype_dissectors = {}

-- The header kind by subtype (Table 6): stream, control, alternative or
-- reserved, generated from the same table the C++ uses.
local header_kind = gen.header_kind

local function dissect_avtpdu(tvb, pinfo, tree)
    local len = tvb:len()
    if len < 1 then
        return 0
    end
    local subtype = tvb(0, 1):uint()
    local kind = header_kind[subtype] or "reserved"
    local version = tvb(1, 1):bitfield(1, 3)
    local name = gen.subtype_names[subtype] or string.format("subtype 0x%02x", subtype)

    pinfo.cols.protocol = "AVTP"
    local root = tree:add(M.proto, tvb(), "IEEE 1722 AVTP: " .. name)

    local header_length = COMMON_HEADER_LENGTH
    if kind == "stream" then
        header_length = version == 1 and STREAM_V1_HEADER_LENGTH or STREAM_V0_HEADER_LENGTH
    end
    if len < header_length then
        root:add(gen.f.avtp_subtype, tvb(0, 1))
        root:add_proto_expert_info(ef_truncated)
        return len
    end

    if kind == "stream" then
        if version == 1 then
            gen.add_stream_v1(root, tvb, 0)
        else
            gen.add_stream_v0(root, tvb, 0)
        end
    elseif kind == "control" then
        gen.add_control(root, tvb, 0)
    elseif kind == "alternative" then
        gen.add_alternative(root, tvb, 0)
    else
        gen.add_common(root, tvb, 0)
        root:add_proto_expert_info(ef_reserved)
    end

    local info = name
    if kind == "stream" then
        local seq = version == 1 and tvb(12, 4):uint() or tvb(2, 1):uint()
        info = string.format("%s seq=%d stream_id=%s", name, seq, tostring(tvb(4, 8):uint64()))
    elseif kind == "control" then
        info = string.format("%s message_type=%d", name, tvb(1, 1):bitfield(4, 4))
    end
    pinfo.cols.info:set(info)

    local header = { subtype = subtype, version = version, kind = kind, header_length = header_length }
    local sub = M.subtype_dissectors[subtype]
    if sub ~= nil then
        sub(tvb, pinfo, root, header)
    elseif len > header_length then
        root:add(gen.f.avtp_payload, tvb(header_length))
    end
    return len
end

function M.proto.dissector(tvb, pinfo, tree)
    return dissect_avtpdu(tvb, pinfo, tree)
end

function M.ipproto.dissector(tvb, pinfo, tree)
    local len = tvb:len()
    pinfo.cols.protocol = "AVTP/UDP"
    local root = tree:add(M.ipproto, tvb(0, math.min(len, IP_AVTPDU_HEADER_LENGTH)), "IEEE 1722 IP AVTPDU")
    if len < IP_AVTPDU_HEADER_LENGTH then
        root:add_proto_expert_info(ef_ip_truncated)
        return len
    end
    gen.add_ip_avtpdu(root, tvb, 0)
    if len > IP_AVTPDU_HEADER_LENGTH then
        dissect_avtpdu(tvb(IP_AVTPDU_HEADER_LENGTH):tvb(), pinfo, tree)
    end
    return len
end

-- Registration and the builtin takeover ------------------------------------

local function builtin(name)
    local ok, handle = pcall(Dissector.get, name)
    if ok then
        return handle
    end
    return nil
end

local function apply_registration()
    local ethertype = DissectorTable.get("ethertype")
    local udp = DissectorTable.get("udp.port")
    if M.proto.prefs.override_builtins then
        ethertype:add(AVTP_ETHERTYPE, M.proto)
        udp:add(UDP_PORT_CONTINUOUS, M.ipproto)
        udp:add(UDP_PORT_DISCRETE, M.ipproto)
    else
        local ieee1722 = builtin("ieee1722")
        if ieee1722 ~= nil then
            ethertype:add(AVTP_ETHERTYPE, ieee1722)
            udp:add(UDP_PORT_CONTINUOUS, ieee1722)
            udp:add(UDP_PORT_DISCRETE, ieee1722)
        else
            ethertype:remove(AVTP_ETHERTYPE, M.proto)
            udp:remove(UDP_PORT_CONTINUOUS, M.ipproto)
            udp:remove(UDP_PORT_DISCRETE, M.ipproto)
        end
    end
end

function M.register()
    M.proto.prefs.override_builtins = Pref.bool(
        "Take over IEEE 1722/1722.1 from the builtin dissectors", true,
        "When on, EtherType 0x22F0 and UDP ports 17220/17221 are dissected by the statusbar "
        .. "AVB dissectors instead of Wireshark's builtin ieee1722/ieee17221. When off, the "
        .. "builtin ieee1722 dissector is put back.")
    M.proto.prefs_changed = apply_registration
    apply_registration()
end

return M
