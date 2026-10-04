-- SPDX-License-Identifier: MIT
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- Per-subtype detail for the AVTP stream, clock and control formats whose
-- layouts the generated module carries (AAF, IEC 61883/AM824, CRF, TSCF,
-- NTSCF, MAAP, AEF, ESCF, EECF): the post hooks that dissect what follows a
-- fixed header (CRF timestamps, AAF samples, IEC 61883 CIP payloads, CVF
-- payload headers) and the
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

--- AM824 (IEC 61883-6) data blocks from `start`, `length` octets, dbs quadlets each
local function add_am824_blocks(tree, tvb, start, length, dbs, pinfo)
    local block = dbs * 4
    local blocks = math.floor(length / block)
    local audio = tree:add(tvb(start, blocks * block), string.format("Audio: %d data blocks x %d channels", blocks, dbs))
    audio:add(gen.f.avtp_am824_data_blocks, tvb(start, blocks * block), blocks):set_generated()
    -- One item per quadlet, its text carrying the label name and the 24-bit
    -- content (as a signed sample for audio labels, as hex otherwise), with
    -- the label, the raw 24 bits and the sample as filterable children.
    local function add(t, at)
        local label = tvb(at, 1):uint()
        local name = am824_label_name(label)
        local is_audio = am824_is_audio(label)
        local item = t:add(gen.f.avtp_am824_quadlet, tvb(at, 4))
        if is_audio then
            item:append_text(string.format(" = %s sample %d", name, tvb(at + 1, 3):int()))
        else
            item:append_text(string.format(" = %s data 0x%06x", name, tvb(at + 1, 3):uint()))
        end
        local label_item = item:add(gen.f.avtp_am824_label, tvb(at, 1))
        if gen.values_am824_label[label] == nil then
            label_item:append_text(string.format(" (%s)", name))
        end
        item:add(gen.f.avtp_am824_data, tvb(at, 4))
        if is_audio then
            item:add(gen.f.avtp_am824_sample, tvb(at + 1, 3))
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

local MPEGTS_SOURCE_PACKET_HEADER = 4
local MPEGTS_PACKET = 188

--- IEC 61883-4 source packets (SPH = 1): dbs*4*2^fn octets each, a source
--- packet header timestamp then an MPEG2 transport packet
local function add_mpegts_source_packets(tree, tvb, start, length, dbs, fn, pinfo)
    local packet = dbs * 4 * (1 << fn)
    local packets = math.floor(length / packet)
    local t = tree:add(tvb(start, packets * packet), string.format("IEC 61883-4: %d source packets x %d octets", packets, packet))
    t:add(gen.f.avtp_mpegts_source_packets, tvb(start, packets * packet), packets):set_generated()
    for i = 0, packets - 1 do
        local at = start + i * packet
        local sp = t:add(tvb(at, packet), string.format("Source packet %d", i))
        sp:add(gen.f.avtp_mpegts_source_packet_timestamp, tvb(at, 4))
        if packet >= MPEGTS_SOURCE_PACKET_HEADER + 4 then
            local ts = sp:add(tvb(at + 4, packet - 4), string.format("MPEG2-TS packet (PID 0x%04x)", tvb(at + 5, 2):uint() % 0x2000))
            ts:add(gen.f.avtp_mpegts_sync_byte, tvb(at + 4, 1))
            ts:add(gen.f.avtp_mpegts_tei, tvb(at + 5, 2))
            ts:add(gen.f.avtp_mpegts_pusi, tvb(at + 5, 2))
            ts:add(gen.f.avtp_mpegts_transport_priority, tvb(at + 5, 2))
            ts:add(gen.f.avtp_mpegts_pid, tvb(at + 5, 2))
            ts:add(gen.f.avtp_mpegts_tsc, tvb(at + 7, 1))
            ts:add(gen.f.avtp_mpegts_afc, tvb(at + 7, 1))
            ts:add(gen.f.avtp_mpegts_cc, tvb(at + 7, 1))
        end
    end
    if length % packet ~= 0 then
        t:add_proto_expert_info(M.ef_partial_frame)
    end
    append_frames(pinfo, "source_packets", packets)
    return start + packets * packet
end

local IEC61883_FMT_AM824 = 0x10
local IEC61883_FMT_MPEGTS = 0x20
local IEC61883_TAG_CIP = 1
local CIP_LENGTH = 8

--- IEC 61883/IIDC: tag 0 is IIDC (opaque video payload); tag 1 carries a CIP
--- header, then AM824 data blocks (FMT 0x10, SPH 0), IEC 61883-4 source
--- packets (FMT 0x20, SPH 1) or an opaque payload for other formats.
function M.post.iec61883(tree, tvb, start, pinfo)
    local v1 = start == 40
    local base = v1 and 32 or 16
    local tag = tvb(base + 6, 1):bitfield(0, 2)
    local declared = tvb(base + 4, 2):uint()
    local available = tvb:len() - start
    if declared > available then
        tree:add_proto_expert_info(M.ef_audio_truncated)
    end
    if tag ~= IEC61883_TAG_CIP or available < CIP_LENGTH then
        return start
    end
    if v1 then
        gen.add_cip_v1(tree, tvb, 0)
    else
        gen.add_cip_v0(tree, tvb, 0)
    end
    local dbs = tvb(start + 1, 1):uint()
    local fn = tvb(start + 2, 1):bitfield(0, 2)
    local sph = tvb(start + 2, 1):bitfield(5, 1) == 1
    local fmt = tvb(start + 4, 1):uint() % 0x40
    local fdf = tvb(start + 5, 1):uint()
    local data_start = start + CIP_LENGTH
    local length = math.max(0, math.min(declared - CIP_LENGTH, available - CIP_LENGTH))
    if fmt == IEC61883_FMT_AM824 and not sph then
        if dbs == 0 or fdf == AM824_FDF_NO_DATA then
            return data_start
        end
        return add_am824_blocks(tree, tvb, data_start, length, dbs, pinfo)
    elseif fmt == IEC61883_FMT_MPEGTS and sph and dbs > 0 then
        return add_mpegts_source_packets(tree, tvb, data_start, length, dbs, fn, pinfo)
    end
    return data_start
end

-- Vendor Specific Format -----------------------------------------------------------

local function vsf_vendor_id(tvb, base)
    local id = tostring(tvb(base, 4):bytes() .. tvb(base + 6, 2):bytes()):lower()
    return (id:gsub("(%x%x)(%x%x)(%x%x)(%x%x)(%x%x)(%x%x)", "%1:%2:%3:%4:%5:%6"))
end

--- VSF (IEEE 1722-2025 14.1.1.2): the OUI-based vendor id is carried as
--- vendor_id_1 (octets 16-19) and vendor_id_2 (octets 22-23) around
--- stream_data_length; add it reassembled as one EUI-48 item spanning both.
function M.post.vsf(tree, tvb, start)
    local base = start == 40 and 32 or 16
    tree:add(gen.f.avtp_vsf_vendor_id, tvb(base, 8), Address.ether(vsf_vendor_id(tvb, base))):set_generated()
    return start
end

-- Compressed Video Format -------------------------------------------------------

local CVF_FORMAT_RFC = 0x02
local CVF_MJPEG, CVF_H264, CVF_JPEG2000, CVF_H265 = 0, 1, 2, 3
local H264_FU_A, H264_FU_B = 28, 29
local H265_FU = 49

--- CVF: the RFC payload header by format_subtype (and the NAL unit header of
--- an H.264/H.265 payload), the rest is the video payload
function M.post.cvf(tree, tvb, start, pinfo)
    local v1 = start == 40
    local base = v1 and 32 or 16
    local format = tvb(base, 1):uint()
    local subtype = tvb(base + 1, 1):uint()
    local len = tvb:len()
    if format ~= CVF_FORMAT_RFC then
        return start
    end
    if subtype == CVF_MJPEG and len >= start + 8 then
        gen.add_cvf_mjpeg(tree, tvb, start)
        return start + 8
    elseif subtype == CVF_H264 and len >= start + 5 then
        gen.add_cvf_h264(tree, tvb, start)
        local nal_type = tvb(start + 4, 1):uint() % 0x20
        local nal = tree:add(tvb(start + 4, 1), string.format("NAL unit header: %s (%d)", gen.values_h264_nal_type[nal_type] or "?", nal_type))
        gen.add_cvf_h264_nal(nal, tvb, start + 4)
        if (nal_type == H264_FU_A or nal_type == H264_FU_B) and len >= start + 6 then
            local fu_type = tvb(start + 5, 1):uint() % 0x20
            local fu = tree:add(tvb(start + 5, 1), string.format("FU header: %s (%d)%s%s", gen.values_h264_nal_type[fu_type] or "?", fu_type,
                tvb(start + 5, 1):bitfield(0, 1) == 1 and " start" or "", tvb(start + 5, 1):bitfield(1, 1) == 1 and " end" or ""))
            gen.add_cvf_h264_fu(fu, tvb, start + 5)
            return start + 6
        end
        return start + 5
    elseif subtype == CVF_H265 and len >= start + 6 then
        gen.add_cvf_h265(tree, tvb, start)
        local nal_type = tvb(start + 4, 2):bitfield(1, 6)
        local nal = tree:add(tvb(start + 4, 2), string.format("NAL unit header: %s (%d)", gen.values_h265_nal_type[nal_type] or "?", nal_type))
        gen.add_cvf_h265_nal(nal, tvb, start + 4)
        if nal_type == H265_FU and len >= start + 7 then
            local fu_type = tvb(start + 6, 1):uint() % 0x40
            local fu = tree:add(tvb(start + 6, 1), string.format("FU header: %s (%d)%s%s", gen.values_h265_nal_type[fu_type] or "?", fu_type,
                tvb(start + 6, 1):bitfield(0, 1) == 1 and " start" or "", tvb(start + 6, 1):bitfield(1, 1) == 1 and " end" or ""))
            gen.add_cvf_h265_fu(fu, tvb, start + 6)
            return start + 7
        end
        return start + 6
    elseif subtype == CVF_JPEG2000 and len >= start + 8 then
        gen.add_cvf_jpeg2000(tree, tvb, start)
        return start + 8
    end
    return start
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
local function iec61883_summary(tvb, version, base, tag)
    local tagbits = tvb(base + 6, 1):bitfield(0, 2)
    if tagbits ~= 1 then
        return string.format("%s IIDC channel=%d %s", tag, tvb(base + 6, 1):uint() % 0x40, stream_summary(tvb, version))
    end
    local cip = base + 8
    if tvb:len() < cip + 8 then
        return string.format("%s (truncated CIP) %s", tag, stream_summary(tvb, version))
    end
    local fmt = tvb(cip + 4, 1):uint() % 0x40
    local dbs = tvb(cip + 1, 1):uint()
    local dbc = tvb(cip + 3, 1):uint()
    if fmt == 0x10 then
        local fdf = tvb(cip + 5, 1):uint()
        return string.format("%s AM824 dbs=%d dbc=%d %s %s", tag, dbs, dbc,
            gen.values_am824_fdf[fdf] or string.format("fdf=0x%02x", fdf), stream_summary(tvb, version))
    end
    return string.format("%s %s dbs=%d dbc=%d %s", tag, gen.values_iec61883_fmt[fmt] or string.format("fmt=0x%02x", fmt),
        dbs, dbc, stream_summary(tvb, version))
end
M.info.iec61883_v0 = function(tvb, version)
    return iec61883_summary(tvb, version, 16, "IEC 61883")
end
M.info.iec61883_v1 = function(tvb, version)
    return iec61883_summary(tvb, version, 32, "IEC 61883 v1")
end
local function cvf_summary(tvb, version, base, tag)
    local subtype = gen.values_cvf_format_subtype[tvb(base + 1, 1):uint()] or string.format("subtype 0x%02x", tvb(base + 1, 1):uint())
    local m = tvb(base + 6, 1):bitfield(3, 1) == 1 and " M" or ""
    return string.format("%s %s%s %s len=%d", tag, subtype, m, stream_summary(tvb, version), tvb(base + 4, 2):uint())
end
M.info.cvf_v0 = function(tvb, version)
    return cvf_summary(tvb, version, 16, "CVF")
end
M.info.cvf_v1 = function(tvb, version)
    return cvf_summary(tvb, version, 32, "CVF v1")
end
local function svf_summary(tvb, version, base, tag)
    return string.format("%s %s line=%d frame=%d%s %s", tag, gen.values_svf_format[tvb(base, 1):uint()] or string.format("format 0x%02x", tvb(base, 1):uint()),
        tvb(base + 2, 2):uint(), tvb(base + 9, 1):uint(), tvb(base + 6, 1):bitfield(3, 1) == 1 and " EF" or "", stream_summary(tvb, version))
end
M.info.svf_v0 = function(tvb, version)
    return svf_summary(tvb, version, 16, "SVF")
end
M.info.svf_v1 = function(tvb, version)
    return svf_summary(tvb, version, 32, "SVF v1")
end
local function rvf_summary(tvb, version, base, tag)
    return string.format("%s %dx%d %s %s fps line=%d%s %s", tag, tvb(base, 2):uint(), tvb(base + 2, 2):uint(),
        gen.values_rvf_pixel_format[tvb(base + 9, 1):uint() % 0x10] or "?", gen.values_rvf_frame_rate[tvb(base + 10, 1):uint()] or "?",
        tvb(base + 14, 2):uint(), tvb(base + 6, 1):bitfield(3, 1) == 1 and " EF" or "", stream_summary(tvb, version))
end
M.info.rvf_v0 = function(tvb, version)
    return rvf_summary(tvb, version, 16, "RVF")
end
M.info.rvf_v1 = function(tvb, version)
    return rvf_summary(tvb, version, 32, "RVF v1")
end
M.info.vsf_v0 = function(tvb, version)
    return string.format("VSF vendor_id=%s %s", vsf_vendor_id(tvb, 16), stream_summary(tvb, version))
end
M.info.vsf_v1 = function(tvb, version)
    return string.format("VSF v1 vendor_id=%s %s", vsf_vendor_id(tvb, 32), stream_summary(tvb, version))
end
M.info.mma_v0 = function(tvb, version)
    return string.format("MMA %s len=%d", stream_summary(tvb, version), tvb(20, 2):uint())
end
M.info.mma_v1 = function(tvb, version)
    return string.format("MMA v1 %s len=%d", stream_summary(tvb, version), tvb(36, 2):uint())
end
M.info.ef_stream_v0 = function(tvb, version)
    return string.format("Experimental stream %s len=%d", stream_summary(tvb, version), tvb(20, 2):uint())
end
M.info.ef_stream_v1 = function(tvb, version)
    return string.format("Experimental stream v1 %s len=%d", stream_summary(tvb, version), tvb(36, 2):uint())
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
