// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#ifndef STATUSBAR_AVB_ENTITY_TX_PCAP_RECORDER_HPP
#define STATUSBAR_AVB_ENTITY_TX_PCAP_RECORDER_HPP

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/pcap/pcap_writer.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace statusbar::avb_entity {

/// Records the entity's OWN transmitted stream frames to an in-memory ring, then
/// flushes them to a libpcap file.
///
/// Why this exists: the stream TX socket uses PACKET_QDISC_BYPASS, so a
/// co-located tcpdump never sees the entity's own egress, and bridges do not
/// flood a reserved stream back to its source port -- the actual on-wire AAF /
/// AM824 / CRF frames we transmit are otherwise impossible to observe without a
/// mirror port. This recorder taps the frames at the socket (the exact bytes
/// handed to sendto, VLAN tag included) and timestamps each with the gPTP time at
/// transmit, so the capture can be opened in Wireshark to inspect what we really
/// put on the wire -- in particular the AVTP presentation timestamps (the media
/// clock we hand a listener).
///
/// RT-safety: record() runs on the media-timer RT thread and only memcpy()s into
/// a pre-allocated arena (no allocation, no I/O). The window closes automatically
/// after `duration_ns` or when the arena fills. write_to_file() does the file I/O
/// and MUST be called from a non-RT context (the entity's main loop) once
/// ready_to_write() returns true.
class TxPcapRecorder
{
  public:
    /// Allocate the capture arena. cap_bytes bounds total memory; snaplen bounds
    /// the bytes captured per frame (the rest is truncated, like tcpdump -s).
    /// duration_ns bounds the wall-clock (gPTP) capture window from the first
    /// recorded frame. Call once before recording.
    void configure(std::string path, size_t cap_bytes, uint32_t snaplen, uint64_t duration_ns)
    {
        path_ = std::move(path);
        snaplen_ = snaplen;
        duration_ns_ = duration_ns;
        arena_.assign(cap_bytes, 0);
        used_ = 0;
        frames_ = 0;
        start_gptp_ns_ = 0;
        armed_ = true;
        ready_to_write_ = false;
        written_ = false;
    }

    /// True while frames are still being accepted.
    [[nodiscard]] auto recording() const noexcept -> bool { return armed_; }

    /// True once the window closed (or arena filled) and the file has not yet been
    /// written. The non-RT main loop polls this and calls write_to_file().
    [[nodiscard]] auto ready_to_write() const noexcept -> bool { return ready_to_write_ && !written_; }

    [[nodiscard]] auto frame_count() const noexcept -> size_t { return frames_; }

    /// RT-safe. Append one transmitted frame (full L2 bytes) timestamped with the
    /// gPTP time at transmit. The first call arms the window. No-op once the
    /// window has closed. Never allocates or does I/O.
    void record(std::span<uint8_t const> frame, uint64_t gptp_ns) noexcept
    {
        if (!armed_) {
            return;
        }
        if (start_gptp_ns_ == 0) {
            start_gptp_ns_ = (gptp_ns == 0) ? 1 : gptp_ns;  // avoid re-arming on a 0 sample
        }
        if (gptp_ns >= start_gptp_ns_ && (gptp_ns - start_gptp_ns_) >= duration_ns_) {
            close_window();
            return;
        }
        uint32_t const cap_len = (frame.size() < snaplen_) ? static_cast<uint32_t>(frame.size()) : snaplen_;
        size_t const need = REC_HDR + cap_len;
        if (used_ + need > arena_.size()) {
            close_window();  // arena full -- stop cleanly, keep what we have
            return;
        }
        uint8_t* p = arena_.data() + used_;
        store_u64(p, gptp_ns);
        store_u32(p + 8, cap_len);
        store_u32(p + 12, static_cast<uint32_t>(frame.size()));  // original (untruncated) length
        span_copy(make_span(arena_, {.start = used_ + REC_HDR, .length = cap_len}), frame.first(cap_len));
        used_ += need;
        ++frames_;
    }

    /// Non-RT. Write the captured frames to `path_` as a libpcap (us-resolution,
    /// LINKTYPE_ETHERNET) file. Safe to call only when ready_to_write().
    [[nodiscard]] auto write_to_file() -> Status
    {
        if (written_) {
            return success();
        }
        written_ = true;
        // FileWriter::open APPENDS to an existing pcap; a capture must be a fresh
        // file, so remove any stale one first (e.g. a prior run's capture).
        (void)std::remove(path_.c_str());
        auto writer = pcap::FileWriter::open(path_);
        if (!writer) {
            return failure(writer.error());
        }
        size_t off = 0;
        while (off + REC_HDR <= used_) {
            uint64_t const ts_ns = load_u64(arena_.data() + off);
            uint32_t const cap_len = load_u32(arena_.data() + off + 8);
            // orig_len at +12 is informational; FileWriter uses the span length.
            off += REC_HDR;
            if (off + cap_len > used_) {
                break;
            }
            (void)writer->write_packet(ts_ns / 1000, std::span<uint8_t const>{arena_.data() + off, cap_len});
            off += cap_len;
        }
        writer->flush();
        return success();
    }

  private:
    static constexpr size_t REC_HDR = 16;  // ts_ns(8) + cap_len(4) + orig_len(4)

    void close_window() noexcept
    {
        armed_ = false;
        ready_to_write_ = true;
    }

    static void store_u64(uint8_t* p, uint64_t v) noexcept
    {
        for (int i = 0; i < 8; ++i) {
            p[i] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    static void store_u32(uint8_t* p, uint32_t v) noexcept
    {
        for (int i = 0; i < 4; ++i) {
            p[i] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    static auto load_u64(uint8_t const* p) noexcept -> uint64_t
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(p[i]) << (8 * i);
        }
        return v;
    }
    static auto load_u32(uint8_t const* p) noexcept -> uint32_t
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(p[i]) << (8 * i);
        }
        return v;
    }

    std::string path_{};
    std::vector<uint8_t> arena_{};
    size_t used_{0};
    size_t frames_{0};
    uint64_t start_gptp_ns_{0};
    uint64_t duration_ns_{0};
    uint32_t snaplen_{0};
    bool armed_{false};
    bool ready_to_write_{false};
    bool written_{false};
};

}  // namespace statusbar::avb_entity

#endif  // STATUSBAR_AVB_ENTITY_TX_PCAP_RECORDER_HPP
