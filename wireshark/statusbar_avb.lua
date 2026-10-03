-- SPDX-License-Identifier: MIT
-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
--
-- Statusbar AVB: IEEE 1722 / IEEE 1722.1 dissectors for Wireshark - loader.
--
-- This is the only file that registers anything. It locates its sibling
-- directory statusbar_avb/, puts it on package.path, and asks the modules
-- to register. Idempotent: a plugin loader that finds this file twice (or
-- a user who also passes it with -X lua_script) registers once.

if _G.statusbar_avb_loaded then
    return
end
_G.statusbar_avb_loaded = true

local source = debug.getinfo(1, "S").source
local script_path = source:sub(1, 1) == "@" and source:sub(2) or source
local base = script_path:match("^(.*)[/\\]") or "."
package.path = base .. "/?.lua;" .. package.path

local avtp = require("statusbar_avb.avtp")
local acf = require("statusbar_avb.acf")
local atdecc = require("statusbar_avb.atdecc")
avtp.register()
acf.register(avtp)
atdecc.register(avtp)
