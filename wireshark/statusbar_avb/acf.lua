-- SPDX-License-Identifier: MIT
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- IEEE 1722-2025 clause 9.4: the ACF messages carried by TSCF and NTSCF.
-- Walks acf_payload_data by acf_msg_length, dissects every clause 9.4 type
-- through the generated field tables (gen/acf_fields.lua, from
-- python/wireshark_schema/acf_table.py), strips each message's padding
-- according to its type's rule, and verifies ACF_CHECKSUM / ACF_CRC
-- trailers against the message before them.
--
-- Written from IEEE Std 1722-2025 and this repository's avtp_acf_*.hpp wire
-- definitions; nothing here derives from Wireshark's own dissectors.

-- Wireshark 4.x executes every .lua file it finds under a plugin folder,
-- including this module, passing it (basename, path); only a require() from
-- the loader statusbar_avb.lua, which passes the dotted module name, may run it.
if type((...)) ~= "string" or not (...):find("^statusbar_avb%.") then
    return
end

local M = {}

local gen = require("statusbar_avb.gen.acf_fields")
local has_integrity, integrity = pcall(require, "statusbar_avb.acf_integrity")

M.proto = Proto.new("avb.acf", "IEEE 1722 ACF messages (statusbar)")
M.proto.fields = gen.acf_fields

local ef_length = ProtoExpert.new("avb.acf.expert.length", "ACF message length is zero or runs past the payload",
    expert.group.MALFORMED, expert.severity.ERROR)
local ef_short = ProtoExpert.new("avb.acf.expert.short", "ACF message shorter than its type's fixed part",
    expert.group.MALFORMED, expert.severity.ERROR)
local ef_bounds = ProtoExpert.new("avb.acf.expert.bounds", "ACF message length outside its type's payload bounds",
    expert.group.PROTOCOL, expert.severity.WARN)
local ef_pad = ProtoExpert.new("avb.acf.expert.pad", "ACF pad larger than the payload", expert.group.MALFORMED,
    expert.severity.ERROR)
local ef_trailer_ok = ProtoExpert.new("avb.acf.expert.trailer_ok", "ACF trailer verifies the preceding message",
    expert.group.CHECKSUM, expert.severity.CHAT)
local ef_trailer_bad = ProtoExpert.new("avb.acf.expert.trailer_bad",
    "ACF trailer does NOT verify the preceding message", expert.group.CHECKSUM, expert.severity.ERROR)
local ef_trailer_alone = ProtoExpert.new("avb.acf.expert.trailer_alone", "ACF trailer with no preceding message",
    expert.group.PROTOCOL, expert.severity.NOTE)
M.proto.experts = { ef_length, ef_short, ef_bounds, ef_pad, ef_trailer_ok, ef_trailer_bad, ef_trailer_alone }

local TSCF = 0x05
local NTSCF = 0x82

--- Octets of padding for a message whose fields are read from `tvb` at `at`
local function implied_pad(spec, tvb, at, padded)
    if spec.pad_mode == "field" then
        return tvb(at + 2, 1):bitfield(0, 2)
    elseif spec.pad_mode == "none" then
        return 0
    end
    local octets
    if spec.name == "parallel" then
        local bits = tvb(at + 3, 1):uint()
        if bits == 0 then
            bits = 256
        end
        octets = math.floor((bits + 7) / 8)
    else -- sensor / sensor_brief: mtv | num_sensors[6:0], sz[7:6] | group
        local count = tvb(at + 2, 1):bitfield(1, 7)
        if count == 0 then
            count = 128
        end
        local size = tvb(at + 3, 1):bitfield(0, 2)
        if size == 0 then
            size = 4
        end
        octets = count * size
    end
    if padded < octets then
        return nil
    end
    return padded - octets
end

--- Dissect one typed message; returns nothing
local function dissect_typed(spec, item, tvb, at, octets, pinfo)
    if octets < spec.length then
        item:add_proto_expert_info(ef_short)
        return
    end
    local quadlets = octets / 4
    local min_total = spec.length / 4 + spec.min_q
    local max_total = spec.max_q and (spec.length / 4 + spec.max_q) or 511
    if quadlets < min_total or quadlets > max_total then
        item:add_proto_expert_info(ef_bounds)
    end
    spec.add(item, tvb, at)
    if spec.fixed_only then
        return
    end
    local padded = octets - spec.length
    local pad = implied_pad(spec, tvb, at, padded)
    if pad == nil or pad > padded then
        item:add_proto_expert_info(ef_pad)
        return
    end
    if padded - pad > 0 then
        item:add(spec.payload, tvb(at + spec.length, padded - pad))
    end
end

--- Verify a Checksum/CRC trailer at `at` against the bytes of the previous message
local function dissect_trailer(msg_type, item, tvb, at, octets, previous)
    if msg_type == gen.MSG_TYPE_CHECKSUM then
        gen.add_acf_checksum(item, tvb, at)
        if previous == nil then
            item:add_proto_expert_info(ef_trailer_alone)
        elseif has_integrity and M.proto.prefs.verify_trailers then
            local ok = integrity.checksum16(previous) == tvb(at + 2, 2):uint()
            item:add(gen.f.acf_checksum_valid, tvb(at + 2, 2), ok)
            item:add_proto_expert_info(ok and ef_trailer_ok or ef_trailer_bad)
        end
        return
    end
    gen.add_acf_crc(item, tvb, at)
    if octets > 4 then
        item:add(gen.f.acf_crc_crc_data, tvb(at + 4, octets - 4))
    end
    if previous == nil then
        item:add_proto_expert_info(ef_trailer_alone)
    elseif has_integrity and M.proto.prefs.verify_trailers and octets == 8 then
        local expected = integrity.crc32(tvb(at + 3, 1):bitfield(4, 4), previous)
        if expected ~= nil then
            local ok = expected == tvb(at + 4, 4):uint()
            item:add(gen.f.acf_crc_valid, tvb(at + 4, 4), ok)
            item:add_proto_expert_info(ok and ef_trailer_ok or ef_trailer_bad)
        end
    end
end

--- Dissect the ACF messages in tvb(start, length); returns the names seen
function M.dissect(tvb, pinfo, tree, start, length)
    local root = tree:add(M.proto, tvb(start, length), "IEEE 1722 ACF messages")
    local at = start
    local stop = start + length
    local previous = nil
    local names = {}
    while at + 2 <= stop do
        local header = tvb(at, 2):uint()
        local msg_type = header >> 9
        local quadlets = header & 0x1FF
        local octets = quadlets * 4
        if quadlets == 0 or at + octets > stop then
            local bad = root:add(tvb(at, stop - at), "ACF message (malformed length)")
            bad:add_proto_expert_info(ef_length)
            break
        end
        local name = gen.values_acf_msg_type[msg_type] or string.format("ACF type 0x%02x", msg_type)
        local item = root:add(tvb(at, octets), string.format("%s (%d octets)", name, octets))
        gen.add_acf_header(item, tvb, at)
        local spec = gen.specs[msg_type]
        if spec ~= nil then
            dissect_typed(spec, item, tvb, at, octets, pinfo)
        elseif msg_type == gen.MSG_TYPE_CHECKSUM or msg_type == gen.MSG_TYPE_CRC then
            dissect_trailer(msg_type, item, tvb, at, octets, previous)
        end
        names[#names + 1] = name
        previous = tvb:raw(at, octets)
        at = at + octets
    end
    return names
end

--- The declared ACF payload length of a TSCF/NTSCF AVTPDU
local function declared_length(tvb, header)
    if header.subtype == TSCF then
        return header.version == 1 and tvb(36, 2):uint() or tvb(20, 2):uint()
    end
    local at = header.version == 1 and 16 or 1
    return tvb(at, 2):uint() & 0x7FF
end

--- The TSCF/NTSCF subtype dissector: ACF messages after the header
local function dissect_control_format(tvb, pinfo, root, header, tree)
    local start = header.rest
    local available = tvb:len() - start
    local length = math.min(declared_length(tvb, header), available)
    if length <= 0 then
        return
    end
    local names = M.dissect(tvb, pinfo, tree, start, length)
    if #names > 0 then
        pinfo.cols.info:append(" [" .. table.concat(names, ", ") .. "]")
    end
end

function M.register(avtp)
    M.proto.prefs.verify_trailers = Pref.bool("Verify ACF Checksum/CRC trailers", true,
        "Check each ACF_CHECKSUM / ACF_CRC message against the message before it and flag the result.")
    avtp.subtype_dissectors[TSCF] = dissect_control_format
    avtp.subtype_dissectors[NTSCF] = dissect_control_format
end

return M
