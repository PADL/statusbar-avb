-- SPDX-License-Identifier: MIT
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- The arithmetic behind the ACF_CHECKSUM (9.4.20) and ACF_CRC (9.4.21)
-- trailers: the ones-complement 16-bit checksum (UDP style, a zero result
-- is a value) and the reflected CRC-32 variants Table 30 names: CRC_ETH
-- (IEEE 802.3, polynomial 0x04C11DB7) and CRC_32P4 (AUTOSAR, 0xF4ACFB13).
-- Runs on Lua 5.2 (Wireshark before 4.4, bit32) and 5.3+ (the native
-- integer operators, compiled through load() so a 5.2 parser never sees
-- them: Wireshark parses every .lua file in the plugin folder).

-- Wireshark 4.x executes every .lua file it finds under a plugin folder,
-- including this module, passing it (basename, path); only a require() from
-- the loader statusbar_avb.lua, which passes the dotted module name, may run it.
if type((...)) ~= "string" or not (...):find("^statusbar_avb%.") then
    return
end

local M = {}

-- bit32 where the host has it (Lua 5.2), else the 5.3+ operators. All values
-- here are unsigned and fit in 32 bits, where both agree.
local bit = bit32
    or load([[return {
        band = function(a, b) return a & b end,
        bor = function(a, b) return a | b end,
        bxor = function(a, b) return a ~ b end,
        lshift = function(a, n) return a << n end,
        rshift = function(a, n) return a >> n end,
    }]])()
local band, bor, bxor, lshift, rshift = bit.band, bit.bor, bit.bxor, bit.lshift, bit.rshift

local function make_table(reflected_poly)
    local t = {}
    for n = 0, 255 do
        local c = n
        for _ = 1, 8 do
            if band(c, 1) == 1 then
                c = bxor(reflected_poly, rshift(c, 1))
            else
                c = rshift(c, 1)
            end
        end
        t[n] = band(c, 0xFFFFFFFF)
    end
    return t
end

local TABLES = {
    [0x0] = make_table(0xEDB88320),  -- CRC_ETH
    [0x1] = make_table(0xC8DF352F),  -- CRC_32P4
}

--- The ones-complement checksum field value for the octets of `str`
function M.checksum16(str)
    local sum = 0
    local n = #str
    for i = 1, n - 1, 2 do
        local hi, lo = str:byte(i, i + 1)
        sum = sum + bor(lshift(hi, 8), lo)
    end
    if n % 2 == 1 then
        sum = sum + lshift(str:byte(n), 8)
    end
    while rshift(sum, 16) ~= 0 do
        sum = band(sum, 0xFFFF) + rshift(sum, 16)
    end
    return bxor(0xFFFF, sum)
end

--- The CRC-32 of crc_type `kind` (0 CRC_ETH, 1 CRC_32P4) over `str`, or nil
--- when the type is not one this module computes
function M.crc32(kind, str)
    local t = TABLES[kind]
    if t == nil then
        return nil
    end
    local crc = 0xFFFFFFFF
    for i = 1, #str do
        crc = bxor(t[band(bxor(crc, str:byte(i)), 0xFF)], rshift(crc, 8))
    end
    return band(bxor(crc, 0xFFFFFFFF), 0xFFFFFFFF)
end

return M
