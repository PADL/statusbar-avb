# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Declarative wire-layout schema for the IEEE 1722 / 1722.1 Wireshark dissectors.

The tables in ``layouts`` describe the frame formats once; ``luagen`` turns them
into the generated Lua field tables under ``wireshark/statusbar_avb/gen/``, and
``decode`` applies the same tables as a reference decoder that the golden-capture
test compares against ``tshark`` output. The schema and this package are MIT;
the Lua they emit is distributed under the GPL with the rest of ``wireshark/``.
"""
