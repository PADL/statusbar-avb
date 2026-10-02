# Statusbar AVB Wireshark dissectors

Lua dissectors for IEEE 1722 (AVTP) and IEEE 1722.1 (ATDECC) frames, over
Ethernet (EtherType 0x22F0) and over UDP (the Annex J IP encapsulation on
ports 17220 and 17221). They take over from Wireshark's builtin `ieee1722`
and `ieee17221` dissectors (preference *Take over IEEE 1722/1722.1 from the
builtin dissectors*, on by default) and expose every field under the
`avb.*` filter namespace: `avb.avtp.*`, `avb.ipavtp.*`, `avb.acf.*` (the clause 9.4 ACF messages inside
TSCF/NTSCF, e.g. `avb.acf.can.can_identifier == 0x18daf110`), and in the
last wave `avb.atdecc.*`.

## License

**This directory is licensed under the GNU General Public License,
version 2 or later (see `LICENSE`), because its contents use the Wireshark
Lua API and the Wireshark project considers dissectors derivative works
of Wireshark.** It is a separate work from the rest of this repository,
which is MIT; the two are merely aggregated here. The schema and the
generator that produce the `statusbar_avb/gen/*.lua` files live in
`python/wireshark_schema/` and remain MIT (the ACF layouts are
`acf_table.py`, the same table the C++ `avtp_acf_*.hpp` headers came from), as
does every C++ source. Do
not copy anything from Wireshark's own sources or dissectors into these
files: everything here is written from the IEEE standards and from this
repository's C++ wire-format definitions.

## Layout

| path | role |
|---|---|
| `statusbar_avb.lua` | the loader: the one file Wireshark must see; registers the dissectors |
| `statusbar_avb/avtp.lua` | hand-written AVTP dissector logic (header kinds, dispatch, preferences) |
| `statusbar_avb/avtp_streams.lua` | per-subtype post hooks (CRF timestamps) and info-column summaries for AAF, AM824, CRF, TSCF, NTSCF, MAAP, AEF, ESCF, EECF |
| `statusbar_avb/acf.lua` | the ACF message walker inside TSCF/NTSCF: every clause 9.4 type, pad rules, Checksum/CRC trailer verification (preference *Verify ACF Checksum/CRC trailers*) |
| `statusbar_avb/acf_integrity.lua` | ones-complement checksum and CRC-32 (Ethernet, AUTOSAR P4) for the trailers; needs Lua 5.3+ (Wireshark 4.4+), otherwise verification is skipped |
| `statusbar_avb/gen/*.lua` | **generated** field tables and layout decoders — do not edit; regenerate with `python3 -m wireshark_schema gen-lua wireshark` from `avb/python/` |

The module files have no side effects beyond defining tables, so a plugin
loader that also scans subdirectories cannot register anything twice;
only `statusbar_avb.lua` registers, and it is idempotent.

## Installing

Copy or link the whole directory into Wireshark's personal plugin folder
and the loader does the rest:

| platform | personal plugin folder |
|---|---|
| Linux | `~/.local/lib/wireshark/plugins/` |
| macOS | `~/.config/wireshark/plugins/` (also `~/.local/lib/wireshark/plugins/`) |
| Windows | `%APPDATA%\Wireshark\plugins\` |

For a one-off run without installing:

    tshark -X lua_script:/path/to/wireshark/statusbar_avb.lua -r capture.pcap

The Debian package `statusbar-avb-wireshark` installs the directory under
`/usr/local/share/statusbar-avb/wireshark/`; the `statusbar-avb-*-wireshark`
tarball carries the same tree for other platforms.

## Testing

`statusbar_avb_wireshark_golden_tool` (built, not installed) writes a
capture of frames produced by this repository's own C++ builders;
`python/wireshark_schema/golden_test.py` runs `tshark -T json` over it with
the loader and compares every `avb.*` field against the reference decoder
generated from the same schema. The ctest `statusbar/avb/wireshark_golden`
skips (exit 77) when `tshark` is not installed.
