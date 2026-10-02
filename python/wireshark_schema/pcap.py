# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Minimal pcap reader: the Ethernet / IPv4 / UDP framing around AVTPDUs."""

from __future__ import annotations

import struct
from dataclasses import dataclass

AVTP_ETHERTYPE = 0x22F0
VLAN_ETHERTYPE = 0x8100
IPV4_ETHERTYPE = 0x0800
UDP_PROTOCOL = 17
AVTP_UDP_PORTS = (17220, 17221)


@dataclass(frozen=True)
class Frame:
    """One captured frame reduced to what the dissector sees."""

    number: int  # 1-based, as Wireshark numbers it
    transport: str  # "ethernet" or "udp"
    avtp: bytes  # the AVTPDU (Ethernet) or the IP AVTPDU incl. its header (UDP)
    udp_dst_port: int | None = None


def read_pcap(path: str) -> list[bytes]:
    """The raw link-layer packets of a classic pcap file."""
    with open(path, "rb") as fh:
        data = fh.read()
    magic = data[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    elif magic == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    else:
        raise ValueError("not a pcap file")
    link_type = struct.unpack(endian + "I", data[20:24])[0]
    if link_type != 1:
        raise ValueError(f"link type {link_type} is not Ethernet")
    packets: list[bytes] = []
    at = 24
    while at + 16 <= len(data):
        _, _, incl_len, _ = struct.unpack(endian + "IIII", data[at : at + 16])
        at += 16
        packets.append(data[at : at + incl_len])
        at += incl_len
    return packets


def avtp_frames(path: str) -> list[Frame]:
    """Every frame of the capture that carries an AVTPDU, with its transport."""
    frames: list[Frame] = []
    for number, packet in enumerate(read_pcap(path), start=1):
        ethertype = struct.unpack(">H", packet[12:14])[0]
        at = 14
        if ethertype == VLAN_ETHERTYPE:
            ethertype = struct.unpack(">H", packet[16:18])[0]
            at = 18
        if ethertype == AVTP_ETHERTYPE:
            frames.append(Frame(number, "ethernet", packet[at:]))
        elif ethertype == IPV4_ETHERTYPE:
            ihl = (packet[at] & 0x0F) * 4
            protocol = packet[at + 9]
            if protocol != UDP_PROTOCOL:
                continue
            udp = at + ihl
            dst_port = struct.unpack(">H", packet[udp + 2 : udp + 4])[0]
            udp_length = struct.unpack(">H", packet[udp + 4 : udp + 6])[0]
            if dst_port in AVTP_UDP_PORTS:
                payload = packet[udp + 8 : udp + udp_length]
                frames.append(Frame(number, "udp", payload, dst_port))
    return frames
