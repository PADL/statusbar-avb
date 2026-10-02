-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- Per-subtype detail for the AVTP stream, clock and control formats whose
-- layouts the generated module carries (AAF, IEC 61883/AM824, CRF, TSCF,
-- NTSCF, MAAP, AEF, ESCF, EECF): the post hooks that dissect what follows a
-- fixed header (CRF timestamps) and the info-column summaries. The field
-- tables and layout decoders are generated; nothing here registers.
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

-- Info-column summaries: spec name -> function(tvb) returning text.
M.info = {}

local function stream_summary(tvb, version, header)
    local seq = version == 1 and tvb(12, 4):uint() or tvb(2, 1):uint()
    return string.format("seq=%d stream_id=0x%s", seq, tostring(tvb(4, 8):bytes()):lower())
end

M.info.aaf_v0 = function(tvb, version)
    local format = gen.values_aaf_format[tvb(16, 1):uint()] or "?"
    local channels = tvb(17, 2):uint() % 0x400
    return string.format("AAF %s %dch %dbit %s", format, channels, tvb(19, 1):uint(), stream_summary(tvb, version))
end
M.info.aaf_v1 = function(tvb, version)
    local format = gen.values_aaf_format[tvb(32, 1):uint()] or "?"
    local channels = tvb(33, 2):uint() % 0x400
    return string.format("AAF v1 %s %dch %dbit %s", format, channels, tvb(35, 1):uint(), stream_summary(tvb, version))
end
M.info.am824_v0 = function(tvb, version)
    return string.format("AM824 dbs=%d dbc=%d fdf=0x%02x %s", tvb(25, 1):uint(), tvb(27, 1):uint(), tvb(29, 1):uint(),
        stream_summary(tvb, version))
end
M.info.am824_v1 = function(tvb, version)
    return string.format("AM824 v1 dbs=%d dbc=%d fdf=0x%02x %s", tvb(41, 1):uint(), tvb(43, 1):uint(),
        tvb(45, 1):uint(), stream_summary(tvb, version))
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
