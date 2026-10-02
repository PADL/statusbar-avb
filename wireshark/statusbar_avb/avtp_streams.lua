-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- Per-subtype detail for the AVTP stream, clock and control formats whose
-- layouts the generated module carries (AAF, IEC 61883/AM824, CRF, TSCF,
-- NTSCF, MAAP, AEF, ESCF, EECF): the post hooks that dissect what follows a
-- fixed header (CRF timestamps, AAF samples, AM824 data blocks) and the
-- info-column summaries. The field tables and layout decoders are generated;
-- nothing here registers (avtp.lua registers the experts and preferences this
-- module exports).
--
-- Written from IEEE Std 1722-2025 and this repository's avtp_*.hpp wire
-- definitions; nothing here derives from Wireshark's own dissectors.

-- Wireshark 4.x executes every .lua file it finds under a plugin folder,
-- including this module, passing it (basename, path); only a require() from
-- the loader statusbar_avb.lua, which passes the dotted module name, may run it.
if type((...)) ~= "string" or not (...):find("^statusbar_avb%.") then
    return
end

local M = {}

local gen = require("statusbar_avb.gen.avtp_fields")

-- Post hooks: name -> function(tree, tvb, start, pinfo) that dissects the
-- bytes from `start` on and returns the offset of the first byte it left
-- alone (the caller shows the rest as payload).
M.post = {}

local CRF_TIMESTAMP_OCTETS = 8

function M.post.crf_timestamps(tree, tvb, start)
    local declared = tvb(start - 4, 2):uint()  -- crf_data_length precedes timestamp_interval
    local available = tvb:len() - start
    local count = math.floor(math.min(declared, available) / CRF_TIMESTAMP_OCTETS)
    for i = 0, count - 1 do
        local at = start + (i * CRF_TIMESTAMP_OCTETS)
        tree:add(gen.f.avtp_crf_timestamp, tvb(at, CRF_TIMESTAMP_OCTETS))
    end
    return start + (count * CRF_TIMESTAMP_OCTETS)
end

-- Audio payloads --------------------------------------------------------------
--
-- AAF PCM (IEEE 1722-2025 7.3.5): channels_per_frame samples per frame, each
-- in the width the format field gives, frames in chronological order. AAF
-- AES3 (7.4.6): two 32-bit subframes per AES3 stream per frame, bits 4-7
-- B C U V, bits 8-31 the audio sample word. AM824 (IEC 61883-6): data blocks
-- of dbs quadlets, each a label octet and 24 bits of data; MBLA (0x40-0x4F)
-- and IEC 60958 conformant (0x00-0x3F) labels carry an audio sample.
--
-- The tree groups the samples by channel (default), by frame, or not at all
-- (preference "Audio sample grouping", owned by avtp.lua, which sets M.prefs).

M.GROUP_BY_CHANNEL = 1
M.GROUP_BY_FRAME = 2
M.GROUP_FLAT = 3
M.prefs = nil

local AAF_FORMAT_USER = 0x00
local AAF_FORMAT_AES3 = 0x05
local AAF_SAMPLE = {
    [0x01] = { field = "avtp_aaf_sample_float32", size = 4 },
    [0x02] = { field = "avtp_aaf_sample_int32", size = 4 },
    [0x03] = { field = "avtp_aaf_sample_int24", size = 3 },
    [0x04] = { field = "avtp_aaf_sample_int16", size = 2 },
}
local AM824_FDF_NO_DATA = 0xFF

M.ef_partial_frame = ProtoExpert.new("avb.avtp.expert.partial_frame",
    "audio payload ends inside a sample frame", expert.group.MALFORMED, expert.severity.WARN)
M.ef_audio_truncated = ProtoExpert.new("avb.avtp.expert.audio_truncated",
    "audio payload shorter than stream_data_length", expert.group.MALFORMED, expert.severity.WARN)
M.experts = { M.ef_partial_frame, M.ef_audio_truncated }

local function grouping()
    return M.prefs and M.prefs.audio_grouping or M.GROUP_BY_CHANNEL
end

--- Lay out `frames` x `channels` items of `size` octets from `start` under
--- `audio`, grouped per the preference; `add(parent, at, frame, channel)`
--- adds one item; `channel_name(channel)` names a channel subtree.
local function add_grouped(audio, tvb, start, frames, channels, size, add, channel_name)
    local mode = grouping()
    local frame_size = channels * size
    if mode == M.GROUP_BY_CHANNEL then
        for c = 0, channels - 1 do
            local t = audio:add(tvb(start, frames * frame_size), string.format("%s (%d samples)", channel_name(c), frames))
            for f = 0, frames - 1 do
                add(t, start + f * frame_size + c * size, f, c)
            end
        end
    elseif mode == M.GROUP_BY_FRAME then
        for f = 0, frames - 1 do
            local t = audio:add(tvb(start + f * frame_size, frame_size), string.format("Frame %d", f))
            for c = 0, channels - 1 do
                add(t, start + f * frame_size + c * size, f, c)
            end
        end
    else
        for f = 0, frames - 1 do
            for c = 0, channels - 1 do
                add(audio, start + f * frame_size + c * size, f, c)
            end
        end
    end
end

local function append_frames(pinfo, what, count)
    if pinfo ~= nil then
        pinfo.cols.info:append(string.format(" %s=%d", what, count))
    end
end

function M.post.aaf_audio(tree, tvb, start, pinfo)
    local v1 = start == 40
    local base = v1 and 32 or 16
    local format = tvb(base, 1):uint()
    local declared = tvb(base + 4, 2):uint()
    local available = tvb:len() - start
    local length = math.min(declared, available)
    local channels, size, add, channel_name
    if format == AAF_FORMAT_AES3 then
        if v1 then
            gen.add_aaf_aes3_v1(tree, tvb, 0)
        else
            gen.add_aaf_aes3_v0(tree, tvb, 0)
        end
        local streams = tvb(base + 1, 2):uint() % 0x400
        channels = 2 * streams
        size = 4
        add = function(t, at, _, c)
            local sf = t:add(gen.f.avtp_aaf_aes3_subframe, tvb(at, 4))
            sf:append_text(string.format(" (stream %d subframe %d)", math.floor(c / 2), (c % 2) + 1))
            sf:add(gen.f.avtp_aaf_aes3_b, tvb(at, 4))
            sf:add(gen.f.avtp_aaf_aes3_c, tvb(at, 4))
            sf:add(gen.f.avtp_aaf_aes3_u, tvb(at, 4))
            sf:add(gen.f.avtp_aaf_aes3_v, tvb(at, 4))
            sf:add(gen.f.avtp_aaf_aes3_audio_sample_word, tvb(at + 1, 3))
        end
        channel_name = function(c)
            return string.format("Stream %d subframe %d", math.floor(c / 2), (c % 2) + 1)
        end
    else
        if v1 then
            gen.add_aaf_pcm_v1(tree, tvb, 0)
        else
            gen.add_aaf_pcm_v0(tree, tvb, 0)
        end
        channels = tvb(base + 1, 2):uint() % 0x400
        local spec = AAF_SAMPLE[format]
        size = spec and spec.size or 0
        if spec then
            local field = gen.f[spec.field]
            add = function(t, at)
                t:add(field, tvb(at, size))
            end
        end
        channel_name = function(c)
            return string.format("Channel %d", c)
        end
    end
    if declared > available then
        tree:add_proto_expert_info(M.ef_audio_truncated)
    end
    if channels == 0 or size == 0 then
        -- user-specified data (or a header without channels): opaque
        if length > 0 then
            tree:add(gen.f.avtp_aaf_pcm_data_payload, tvb(start, length))
        end
        return start + length
    end
    local frame_size = channels * size
    local frames = math.floor(length / frame_size)
    local audio = tree:add(tvb(start, frames * frame_size), string.format("Audio: %d frames x %d %s, %d octets each",
        frames, channels, format == AAF_FORMAT_AES3 and "subframes" or "channels", size))
    audio:add(gen.f.avtp_aaf_frames, tvb(start, frames * frame_size), frames):set_generated()
    add_grouped(audio, tvb, start, frames, channels, size, add, channel_name)
    if length % frame_size ~= 0 then
        audio:add_proto_expert_info(M.ef_partial_frame)
    end
    append_frames(pinfo, "frames", frames)
    return start + frames * frame_size
end

local function am824_label_name(label)
    local exact = gen.values_am824_label[label]
    if exact then
        return exact
    end
    if label < 0x40 then
        return "IEC 60958 conformant"
    elseif label < 0x50 then
        return "MBLA"
    elseif label < 0x60 then
        return "one-bit audio"
    elseif label < 0x70 then
        return "high-precision MBLA"
    elseif label < 0x80 then
        return "reserved"
    elseif label < 0xC0 then
        return "MIDI/ancillary conformant"
    end
    return "reserved"
end

local function am824_is_audio(label)
    return label < 0x50
end

function M.post.am824_audio(tree, tvb, start, pinfo)
    local v1 = start == 48
    local base = v1 and 36 or 20
    local dbs = tvb(start - 7, 1):uint()
    local fdf = tvb(start - 3, 1):uint()
    local declared = tvb(base, 2):uint() - 8  -- stream_data_length counts the CIP header
    local available = tvb:len() - start
    local length = math.max(0, math.min(declared, available))
    if declared > available then
        tree:add_proto_expert_info(M.ef_audio_truncated)
    end
    if dbs == 0 or fdf == AM824_FDF_NO_DATA then
        return start
    end
    local block = dbs * 4
    local blocks = math.floor(length / block)
    local audio = tree:add(tvb(start, blocks * block), string.format("Audio: %d data blocks x %d channels", blocks, dbs))
    audio:add(gen.f.avtp_am824_data_blocks, tvb(start, blocks * block), blocks):set_generated()
    local function add(t, at)
        local label = tvb(at, 1):uint()
        local item
        if am824_is_audio(label) then
            item = t:add(gen.f.avtp_am824_sample, tvb(at + 1, 3))
        else
            item = t:add(gen.f.avtp_am824_data, tvb(at, 4))
        end
        local name = am824_label_name(label)
        item:append_text(string.format(" [%s]", name))
        local label_item = item:add(gen.f.avtp_am824_label, tvb(at, 1))
        if gen.values_am824_label[label] == nil then
            label_item:append_text(string.format(" (%s)", name))
        end
    end
    add_grouped(audio, tvb, start, blocks, dbs, 4, add, function(c)
        return string.format("Channel %d", c)
    end)
    if length % block ~= 0 then
        audio:add_proto_expert_info(M.ef_partial_frame)
    end
    append_frames(pinfo, "blocks", blocks)
    return start + blocks * block
end

-- Info-column summaries: spec name -> function(tvb) returning text.
M.info = {}

local function stream_summary(tvb, version, header)
    local seq = version == 1 and tvb(12, 4):uint() or tvb(2, 1):uint()
    return string.format("seq=%d stream_id=0x%s", seq, tostring(tvb(4, 8):bytes()):lower())
end

local function aaf_summary(tvb, version, base, tag)
    local code = tvb(base, 1):uint()
    local format = gen.values_aaf_format[code] or "?"
    local count = tvb(base + 1, 2):uint() % 0x400
    if code == AAF_FORMAT_AES3 then
        local rate = gen.values_aaf_nsr[tvb(base + 1, 1):bitfield(0, 4)] or "?"
        return string.format("%s AES3 %d streams %s %s", tag, count, rate, stream_summary(tvb, version))
    end
    local rate = gen.values_aaf_nsr[tvb(base + 1, 1):bitfield(0, 4)] or "?"
    return string.format("%s %s %dch %dbit %s %s", tag, format, count, tvb(base + 3, 1):uint(), rate,
        stream_summary(tvb, version))
end
M.info.aaf_v0 = function(tvb, version)
    return aaf_summary(tvb, version, 16, "AAF")
end
M.info.aaf_v1 = function(tvb, version)
    return aaf_summary(tvb, version, 32, "AAF v1")
end
local function am824_summary(tvb, version, base, tag)
    local fdf = tvb(base + 5, 1):uint()
    return string.format("%s dbs=%d dbc=%d %s %s", tag, tvb(base + 1, 1):uint(), tvb(base + 3, 1):uint(),
        gen.values_am824_fdf[fdf] or string.format("fdf=0x%02x", fdf), stream_summary(tvb, version))
end
M.info.am824_v0 = function(tvb, version)
    return am824_summary(tvb, version, 24, "AM824")
end
M.info.am824_v1 = function(tvb, version)
    return am824_summary(tvb, version, 40, "AM824 v1")
end
M.info.tscf_v0 = function(tvb, version)
    return string.format("TSCF %s acf=%d octets", stream_summary(tvb, version), tvb(20, 2):uint())
end
M.info.tscf_v1 = function(tvb, version)
    return string.format("TSCF v1 %s acf=%d octets", stream_summary(tvb, version), tvb(36, 2):uint())
end
M.info.ntscf_v0 = function(tvb)
    return string.format("NTSCF seq_lsb=%d acf=%d octets stream_id=0x%s", tvb(3, 1):uint(), tvb(1, 2):uint() % 0x800,
        tostring(tvb(4, 8):bytes()):lower())
end
M.info.ntscf_v1 = function(tvb)
    return string.format("NTSCF v1 seq=%d acf=%d octets stream_id=0x%s", tvb(4, 4):uint(), tvb(16, 2):uint() % 0x800,
        tostring(tvb(20, 8):bytes()):lower())
end
M.info.crf_v0 = function(tvb)
    local kind = gen.values_crf_type[tvb(3, 1):uint()] or "?"
    return string.format("CRF %s base_frequency=%d Hz seq=%d", kind, tvb(12, 4):uint() % 0x20000000, tvb(2, 1):uint())
end
M.info.crf_v1 = function(tvb)
    local kind = gen.values_crf_type[tvb(18, 1):uint()] or "?"
    return string.format("CRF v1 %s base_frequency=%d Hz seq=%d", kind, tvb(28, 4):uint() % 0x20000000,
        tvb(4, 4):uint())
end
M.info.maap = function(tvb)
    local kind = gen.values_maap_message_type[tvb(1, 1):bitfield(4, 4)] or "MAAP"
    return string.format("%s start=%s count=%d", kind, tostring(tvb(12, 6):ether()), tvb(18, 2):uint())
end
M.info.aef_continuous = function(tvb)
    return string.format("AEF continuous %s key_id=0x%s", gen.values_aef_enc[tvb(1, 1):bitfield(4, 4)] or "?",
        tostring(tvb(4, 8):bytes()):lower())
end
M.info.aef_discrete = function(tvb)
    return string.format("AEF discrete %s key_id=0x%s", gen.values_aef_enc[tvb(1, 1):bitfield(4, 4)] or "?",
        tostring(tvb(4, 8):bytes()):lower())
end
M.info.escf = function(tvb)
    return string.format("ESCF %s key_id=0x%s", gen.values_escf_sig[tvb(1, 1):bitfield(4, 4)] or "?",
        tostring(tvb(4, 8):bytes()):lower())
end
M.info.eecf = function(tvb)
    return string.format("EECF %s key_id=0x%s", gen.values_eecf_enc[tvb(1, 1):bitfield(4, 4)] or "?",
        tostring(tvb(4, 8):bytes()):lower())
end

return M
