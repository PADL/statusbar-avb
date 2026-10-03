# Statusbar AVB Wireshark dissectors

Lua dissectors for IEEE 1722 (AVTP) and IEEE 1722.1 (ATDECC) frames, over
Ethernet (EtherType 0x22F0) and over UDP (the Annex J IP encapsulation on
ports 17220 and 17221). They take over from Wireshark's builtin `ieee1722`
and `ieee17221` dissectors (preference *Take over IEEE 1722/1722.1 from the
builtin dissectors*, on by default) and expose every field under the
`avb.*` filter namespace: `avb.avtp.*` (headers, and the audio itself: AAF
PCM samples as `avb.avtp.aaf.sample_int16/int24/int32/float32`, AES3
subframes with their B/C/U/V bits and `avb.avtp.aaf.aes3.audio_sample_word`,
AM824 quadlets as `avb.avtp.am824.label` + `.sample` or `.data`; grouped per
channel, per frame or flat by the preference *Audio sample grouping*;
IEC 61883-4 transport packets as `avb.avtp.mpegts.*`; compressed video
headers as `avb.avtp.cvf.mjpeg.*`, `.h264.*`, `.h265.*`, `.jpeg2000.*`; SDI,
raw video and vendor-specific headers as `avb.avtp.svf.*`, `.rvf.*`, `.vsf.*`),
`avb.ipavtp.*`, `avb.acf.*` (the clause 9.4 ACF messages inside
TSCF/NTSCF, e.g. `avb.acf.can.can_identifier == 0x18daf110`), and
`avb.atdecc.*` (ADP, ACMP and AECP: every AEM command payload, every
descriptor a READ_DESCRIPTOR response can carry including its counted
trailers, control values, the JDKS log/IPv4 vendor blobs, Address Access
TLVs, Vendor Unique; e.g. `avb.atdecc.aem.command_type == 0x0004`).
Commands and responses are paired on (controller, sequence) and shown as
*Response in* / *Response to* / *Response time* generated items.

## License

**This directory is licensed under the GNU General Public License,
version 2 or later (see `LICENSE`), because its contents use the Wireshark
Lua API and the Wireshark project considers dissectors derivative works
of Wireshark.** It is a separate work from the rest of this repository,
which is MIT; the two are merely aggregated here. The schema and the
generator that produce the `statusbar_avb/gen/*.lua` files live in
`python/wireshark_schema/` and remain MIT (the ACF layouts are
`acf_table.py`, the same table the C++ `avtp_acf_*.hpp` headers came from;
the ATDECC layouts are `atdecc_table.py`, extracted from the C++
`atdecc_*.hpp` wire structs), as does every C++ source. Do
not copy anything from Wireshark's own sources or dissectors into these
files: everything here is written from the IEEE standards and from this
repository's C++ wire-format definitions.

## Layout

| path | role |
|---|---|
| `statusbar_avb.lua` | the loader: the one file Wireshark must see; registers the dissectors |
| `statusbar_avb/avtp.lua` | hand-written AVTP dissector logic (header kinds, dispatch, preferences) |
| `statusbar_avb/avtp_streams.lua` | per-subtype post hooks (CRF timestamps; AAF PCM samples and AES3 subframes; IEC 61883: IIDC, CIP, AM824 data blocks, IEC 61883-4 MPEG2-TS source packets; CVF: MJPEG, H.264 and H.265 NAL/FU headers, JPEG 2000; audio grouped by channel or by frame per the preference *Audio sample grouping*) and info-column summaries for every stream subtype (IEC 61883, MMA, AAF, CVF, CRF, TSCF, SVF, RVF, VSF, EF, NTSCF) and MAAP, AEF, ESCF, EECF |
| `statusbar_avb/acf.lua` | the ACF message walker inside TSCF/NTSCF: every clause 9.4 type, pad rules, Checksum/CRC trailer verification (preference *Verify ACF Checksum/CRC trailers*) |
| `statusbar_avb/atdecc.lua` | IEEE 1722.1: ADP, ACMP, AECP (AEM payloads and descriptors by table, control values, Address Access TLVs, Vendor Unique), command/response pairing |
| `statusbar_avb/acf_integrity.lua` | ones-complement checksum and CRC-32 (Ethernet, AUTOSAR P4) for the trailers; needs Lua 5.3+ (Wireshark 4.4+), otherwise verification is skipped |
| `statusbar_avb/gen/*.lua` | **generated** field tables and layout decoders — do not edit; regenerate with `python3 -m wireshark_schema gen-lua wireshark` from `avb/python/` |

Wireshark executes every `.lua` under a plugin folder, modules included,
passing each `(basename, path)`; `require()` from the loader passes the
dotted module name instead, and every module returns immediately unless it
sees that, so nothing is defined or registered twice. Only
`statusbar_avb.lua` registers, and it is idempotent.

## Installing

Put the whole directory (or a symlink to it) anywhere under Wireshark's
personal plugin folder; Wireshark 4.x scans it recursively, finds
`statusbar_avb.lua`, and the loader does the rest. The other `.lua` files
are also executed by that scan and return at once, so only the loader
registers anything.

| platform | personal plugin folder |
|---|---|
| Linux | `~/.local/lib/wireshark/plugins/` |
| macOS | `~/.config/wireshark/plugins/` (also `~/.local/lib/wireshark/plugins/`) |
| Windows | `%APPDATA%\Wireshark\plugins\` |

From a checkout (macOS shown; use the Linux folder there):

    mkdir -p ~/.config/wireshark/plugins
    ln -s "$PWD/avb/wireshark" ~/.config/wireshark/plugins/statusbar-avb

From the tarball, which carries the tree under
`usr/local/share/statusbar-avb/wireshark/`:

    mkdir -p ~/.config/wireshark/plugins/statusbar-avb
    tar xzf statusbar-avb-<ver>-<sys>-wireshark.tar.gz \
        -C ~/.config/wireshark/plugins/statusbar-avb --strip-components=5

The Debian package `statusbar-avb-wireshark` installs the tree under
`/usr/local/share/statusbar-avb/wireshark/`, which is not a plugin folder;
link it for each user who wants it (or into the global plugin folder):

    ln -s /usr/local/share/statusbar-avb/wireshark ~/.local/lib/wireshark/plugins/statusbar-avb

Confirm with `tshark -G plugins | grep statusbar_avb` and restart Wireshark.
For a one-off run without installing:

    tshark -X lua_script:/path/to/wireshark/statusbar_avb.lua -r capture.pcap

### Building the tarball and the deb

Packaging is part of the standalone avb build (not the umbrella build), and
only the `wireshark` component is needed, so from the umbrella directory:

    cmake -S avb -B avb/build -G Ninja --toolchain core/cmake/toolchain-clang.cmake
    cmake --build avb/build
    cpack --config avb/build/CPackConfig.cmake -B avb/build -G TGZ -D CPACK_COMPONENTS_ALL=wireshark

which writes `avb/build/statusbar-avb-<ver>-<sys>-wireshark.tar.gz` (`-G DEB`
on Linux for the `.deb`; `container-build.sh` produces it for the Pi nodes).

## Testing

`statusbar_avb_wireshark_golden_tool` (built, not installed) writes a
capture of frames produced by this repository's own C++ builders;
`python/wireshark_schema/golden_test.py` runs `tshark -T json` over it with
the loader (once passed with `-X lua_script`, once found by itself in a
staged personal plugin folder) and compares every `avb.*` field against
the reference decoder generated from the same schema. The ctest `statusbar/avb/wireshark_golden`
skips (exit 77) when `tshark` is not installed.
