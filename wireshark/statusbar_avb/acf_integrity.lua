-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- The arithmetic behind the ACF_CHECKSUM (9.4.20) and ACF_CRC (9.4.21)
-- trailers: the ones-complement 16-bit checksum (UDP style, a zero result
-- is a value) and the reflected CRC-32 variants Table 30 names: CRC_ETH
-- (IEEE 802.3, polynomial 0x04C11DB7) and CRC_32P4 (AUTOSAR, 0xF4ACFB13).
-- Uses the Lua 5.3+ integer operators (Wireshark 4.4 and later); the ACF
-- module loads this file with pcall and simply skips verification when the
-- host Lua is older.

local M = {}

local function make_table(reflected_poly)
    local t = {}
    for n = 0, 255 do
        local c = n
        for _ = 1, 8 do
            if c & 1 == 1 then
                c = reflected_poly ~ (c >> 1)
            else
                c = c >> 1
            end
        end
        t[n] = c & 0xFFFFFFFF
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
        sum = sum + ((hi << 8) | lo)
    end
    if n % 2 == 1 then
        sum = sum + (str:byte(n) << 8)
    end
    while (sum >> 16) ~= 0 do
        sum = (sum & 0xFFFF) + (sum >> 16)
    end
    return 0xFFFF ~ sum
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
        crc = t[(crc ~ str:byte(i)) & 0xFF] ~ (crc >> 8)
    end
    return (crc ~ 0xFFFFFFFF) & 0xFFFFFFFF
end

return M
