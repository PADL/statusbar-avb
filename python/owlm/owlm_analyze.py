#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
#
# Runtime dependencies (apt-installed when this script ships in
# statusbar-avb.deb): python3-numpy, python3-pandas,
# python3-matplotlib, python3-pyarrow. The PEP 723 block below is kept
# for `uv run` invocation in dev trees that don't have those packages.
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy", "pandas", "matplotlib", "pyarrow"]
# ///
"""Joint analysis for OWLM per-packet CSVs produced by owlm_tool --csv-output.

The CSV files are typically huge (an 8-hour run at 1000 packets/sec is
about 6 GB). The plotting / joint-summary commands therefore operate on
a *summary* file produced by the `summarize` subcommand, which streams
the CSV in chunks and pre-aggregates it into a fixed number of pixel
buckets. The summary file is small (a few hundred KB) and lets `plot`
and `join` render quickly without ever holding the raw CSV in memory.

Both plain `.csv` and gzipped `.csv.gz` inputs are accepted; gzipped
files are decompressed in a streaming fashion. For `.gz` inputs the
parallel byte-range pass is skipped (gzip is a non-seekable stream
format), so summarize runs single-threaded — still streaming, just
slower than a multi-worker pass over an uncompressed CSV.

`.colbin` inputs are also accepted: a mmap-friendly packed binary
format written by `owlm_tool --bin-output`. The file is `np.memmap`'d
and iterated in chunks. Much faster than CSV parsing, with no
compression but ~half the raw size. See statusbar/colbin/ for the
format definition.
"""

from __future__ import annotations

import argparse
import io
import json
import multiprocessing as mp
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd


CSV_DTYPES = {
    "rx_gptp_ns": "int64",
    "presentation_time_ns": "int64",
    "latency_ns": "int64",
    "sender_eui64": "string",
    "sequence": "uint32",
    "interval_us": "uint32",
    "role": "category",
}

# ----------------------------------------------------------------------
# colbin (statusbar/colbin/) — mmap-friendly packed binary records.
# Format definition lives in statusbar/colbin/colbin.hpp.
# ----------------------------------------------------------------------

COLBIN_MAGIC = b"STBCOLBN"
COLBIN_ENDIAN_MARKER = 0x12345678
COLBIN_VERSION = 1
COLBIN_HEADER_PREAMBLE = 64

COLBIN_TYPE_TO_DTYPE = {
    "i8": "<i1",
    "u8": "<u1",
    "i16": "<i2",
    "u16": "<u2",
    "i32": "<i4",
    "u32": "<u4",
    "i64": "<i8",
    "u64": "<u8",
    "f32": "<f4",
    "f64": "<f8",
    "b8": "?",
}

# Stable ordering of PacketRole enum values; mirrors role_to_string()
# in statusbar/udptun/udptun_csv_record.cpp.
COLBIN_ROLE_NAMES = np.array(
    [
        "self_primary",  # 0
        "self_redundant",  # 1
        "self_legacy",  # 2
        "remote_primary",  # 3
        "remote_redundant",  # 4
        "remote_legacy",  # 5
    ],
    dtype=object,
)

# gPTP timestamps are TAI seconds since the PTP epoch (1970-01-01 00:00:00
# TAI). Subtracting this offset gives UTC POSIX seconds. 37 has been the
# TAI-UTC offset since the 2017-01-01 leap second.
TAI_UTC_OFFSET_SEC = 37
# Pacific Daylight Time = UTC-7 (PST would be UTC-8). The lab's wall
# clock is PDT during the months the test runs cover.
# TODO: parameterize via a --tz CLI argument (default to the host's
# local timezone). The literal "PDT" string is currently baked into
# format output and self-tests; renaming or generalizing requires
# updating those call sites and tests in lockstep.
PDT_OFFSET_HOURS = -7


def _tai_ns_to_pdt_str(tai_ns: int) -> str:
    """Format a gPTP TAI nanosecond timestamp as a PDT date+time string
    (``YYYY-MM-DD HH:MM:SS PDT``)."""
    import datetime as _dt

    utc_posix_sec = tai_ns / 1e9 - TAI_UTC_OFFSET_SEC
    pdt = _dt.datetime.fromtimestamp(
        utc_posix_sec,
        tz=_dt.timezone(_dt.timedelta(hours=PDT_OFFSET_HOURS)),
    )
    return pdt.strftime("%Y-%m-%d %H:%M:%S PDT")


# Default chunk size for streaming reads. 200 k rows ≈ 50 MB of CSV ≈
# ~20 MB of dataframe in memory — chosen to amortize pandas overhead
# without spiking RSS.
DEFAULT_CHUNKSIZE = 200_000

# Default target plot width. ~2000 buckets covers an A4 PDF at 250 DPI
# without overlap, and an 8-hour run with 1 ms tx interval reduces from
# 29 M raw rows to one bucket per ~14 seconds.
DEFAULT_PLOT_WIDTH = 2000

# Per-bucket log-spaced latency histogram. 64 log bins between
# 100 ns and 1 s (~9 bins / decade) plus two overflow bins at each end
# for negatives and very-large outliers. 66 bins × int32 ≈ 264 B per
# bucket; 2000 buckets ≈ 530 KB. Carries enough information to compute
# per-bucket percentiles (rolling pNN) and a global distribution
# (histogram panel) without keeping any raw row in memory.
HIST_LOG_LOW_NS = 100  # 100 ns
HIST_LOG_HIGH_NS = 1_000_000_000  # 1 s
HIST_N_LOG_BINS = 64
HIST_EDGES_NS = np.logspace(
    np.log10(HIST_LOG_LOW_NS),
    np.log10(HIST_LOG_HIGH_NS),
    HIST_N_LOG_BINS + 1,
)
# Geometric-mean bin centers for percentile reconstruction.
HIST_BIN_CENTERS_NS = np.sqrt(HIST_EDGES_NS[:-1] * HIST_EDGES_NS[1:])
# Total stored bins: [neg overflow] + log-spaced + [pos overflow]
HIST_N_BINS = HIST_N_LOG_BINS + 2
HIST_NEG_BIN = 0
HIST_POS_OVERFLOW_BIN = HIST_N_BINS - 1


def _hist_bin_indices(lat_ns: np.ndarray) -> np.ndarray:
    """Map an array of latency-ns to bin indices in [0, HIST_N_BINS)."""
    # Default: place into log-spaced bins (shifted by 1 to leave bin 0
    # for the negative-overflow bucket).
    log_idx = np.searchsorted(HIST_EDGES_NS, lat_ns, side="right") - 1
    log_idx = np.clip(log_idx, 0, HIST_N_LOG_BINS - 1)
    idx = log_idx + 1
    idx = np.where(lat_ns <= 0, HIST_NEG_BIN, idx)
    idx = np.where(lat_ns >= HIST_LOG_HIGH_NS, HIST_POS_OVERFLOW_BIN, idx)
    return idx


def _bucket_percentiles(hist: np.ndarray, percentiles: list[float]) -> np.ndarray:
    """Compute approximate percentiles for each bucket from its histogram.

    hist shape: (n_buckets, HIST_N_BINS). Returns (n_buckets, len(percentiles))
    in ns. Empty buckets (sum == 0) produce NaN.
    """
    cum = np.cumsum(hist, axis=1)
    total = cum[:, -1:]
    out = np.full((hist.shape[0], len(percentiles)), np.nan, dtype=np.float64)
    nonempty = total[:, 0] > 0
    if not nonempty.any():
        return out
    # Synthetic "value" per bin: 0 for negative-overflow, geometric-mean
    # center for log bins, HIST_LOG_HIGH_NS for positive-overflow.
    bin_value_ns = np.concatenate(
        (
            np.array([0.0]),
            HIST_BIN_CENTERS_NS,
            np.array([float(HIST_LOG_HIGH_NS)]),
        )
    )
    for j, p in enumerate(percentiles):
        target = total[:, 0] * (p / 100.0)
        # For each row find the first column where cum >= target
        idx = (cum >= target[:, None]).argmax(axis=1)
        out[nonempty, j] = bin_value_ns[idx[nonempty]]
    return out


def _bucket_percentiles_interp(
    hist: np.ndarray, percentiles: list[float]
) -> np.ndarray:
    """Percentiles per bucket with log-linear interpolation inside the
    selected bin.

    Same shape contract as :func:`_bucket_percentiles`, but instead of
    returning the bin's geometric-mean center, this assumes a log-uniform
    density within each populated log bin and interpolates accordingly:

        value = edge_lo * (edge_hi / edge_lo) ** frac

    where ``frac`` is the fraction of the bin's count required to reach
    the target cumulative ``p/100 * total``. For a bin containing the
    whole population this reduces to ``edge_lo * (edge_hi/edge_lo)**(p/100)``,
    matching the bin-center result only when ``p = 50``; for p=99.99
    where the target sits at the very top of the highest populated bin,
    interpolation reports a value near ``edge_hi`` instead of the bin
    center, eliminating the ~half-bin-width systematic underestimate.

    The negative-overflow bin (≤ 0 ns) reports 0; the positive-overflow
    bin reports ``HIST_LOG_HIGH_NS`` (no upper edge available).
    """
    cum = np.cumsum(hist, axis=1)
    total = cum[:, -1].astype(np.float64)
    n_buckets = hist.shape[0]
    out = np.full((n_buckets, len(percentiles)), np.nan, dtype=np.float64)
    nonempty = total > 0
    if not nonempty.any():
        return out
    # cum_lo[r, i] = count below bin i in row r (= 0 for i=0).
    cum_lo = np.concatenate(
        [np.zeros((n_buckets, 1), dtype=cum.dtype), cum[:, :-1]],
        axis=1,
    )
    rows = np.arange(n_buckets)
    for j, p in enumerate(percentiles):
        target = total * (p / 100.0)
        idx = (cum >= target[:, None]).argmax(axis=1)  # (n_buckets,)
        bin_count = hist[rows, idx].astype(np.float64)
        need = target - cum_lo[rows, idx].astype(np.float64)
        frac = np.divide(need, bin_count, out=np.zeros_like(need), where=bin_count > 0)
        frac = np.clip(frac, 0.0, 1.0)
        # idx == 0 → negative-overflow; idx == HIST_POS_OVERFLOW_BIN → positive-overflow.
        # Otherwise idx - 1 indexes the log bin (which has edges HIST_EDGES_NS[k..k+1]).
        log_idx = np.clip(idx - 1, 0, HIST_N_LOG_BINS - 1)
        edge_lo = HIST_EDGES_NS[log_idx]
        edge_hi = HIST_EDGES_NS[log_idx + 1]
        value = edge_lo * np.power(edge_hi / edge_lo, frac)
        value = np.where(idx == HIST_NEG_BIN, 0.0, value)
        value = np.where(idx == HIST_POS_OVERFLOW_BIN, float(HIST_LOG_HIGH_NS), value)
        out[nonempty, j] = value[nonempty]
    return out


# ----------------------------------------------------------------------
# CSV helpers (legacy, used by `events` subcommand and tests)
# ----------------------------------------------------------------------


def load_owlm_csv(path: Path) -> pd.DataFrame:
    """Load an owlm_tool input with the documented schema. Materializes
    the entire file — only safe for small inputs. Use `summarize_csv`
    for multi-GB inputs. Accepts .csv, .csv.gz, and .colbin."""
    if _is_colbin(path):
        return pd.concat(
            list(_iter_colbin_chunks(path, chunksize=1_000_000)), ignore_index=True
        )
    return pd.read_csv(path, dtype=CSV_DTYPES)


def remote_primary(df: pd.DataFrame) -> pd.DataFrame:
    """Return rows where role is remote_primary or remote_legacy (cross-clock observations)."""
    return df[df["role"].isin(["remote_primary", "remote_legacy"])]


def _pair_id_from_eui64(s: str) -> str:
    """Zero the redundancy mid bytes so primary (mid=00:00) and redundant
    (mid=00:01) copies of the same logical sender share a pair id."""
    parts = s.split(":")
    if len(parts) != 8:
        return s
    parts[3] = "00"
    parts[4] = "00"
    return ":".join(parts)


# ----------------------------------------------------------------------
# Streaming summarizer
# ----------------------------------------------------------------------


@dataclass
class SummaryMeta:
    """Sidecar metadata for a summary parquet."""

    t0_ns: int
    t1_ns: int
    bucket_ns: int
    n_buckets: int
    wcl_ns: int
    total_rows: int
    source_csv: str

    def to_dict(self) -> dict:
        return {
            "t0_ns": int(self.t0_ns),
            "t1_ns": int(self.t1_ns),
            "bucket_ns": int(self.bucket_ns),
            "n_buckets": int(self.n_buckets),
            "wcl_ns": int(self.wcl_ns),
            "total_rows": int(self.total_rows),
            "source_csv": self.source_csv,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "SummaryMeta":
        return cls(
            t0_ns=int(d["t0_ns"]),
            t1_ns=int(d["t1_ns"]),
            bucket_ns=int(d["bucket_ns"]),
            n_buckets=int(d["n_buckets"]),
            wcl_ns=int(d["wcl_ns"]),
            total_rows=int(d["total_rows"]),
            source_csv=str(d["source_csv"]),
        )


CSV_COLUMNS = [
    "rx_gptp_ns",
    "presentation_time_ns",
    "latency_ns",
    "sender_eui64",
    "sequence",
    "interval_us",
    "role",
]


def _iter_chunks_tolerant(reader, label: str):
    """Wrap a pandas chunked reader so a parser error (e.g. a truncated
    final row, missing trailing newline, gzip EOF mid-stream) is treated
    as end-of-stream rather than a fatal error. Useful when the CSV
    came from a tool that was killed mid-write.

    The chunk that triggered the error is dropped (pandas raises mid-
    chunk before delivering its rows), so the realistic worst-case
    tail loss is one chunksize worth of rows."""
    try:
        for chunk in reader:
            yield chunk
    except (ValueError, EOFError, pd.errors.ParserError, OSError) as e:
        print(
            f"warning [{label}]: stopping CSV read early "
            f"({type(e).__name__}: {e}) — input may be truncated",
            file=sys.stderr,
        )


@dataclass
class _ScanMeta:
    """Output of the first pass over the CSV — time range and per-pair
    sequence ranges needed to size the per-pair bitmaps in pass 2."""

    t0_ns: int
    t1_ns: int
    total_rows: int
    # pair_id -> (min_seq, max_seq)
    pair_seq_ranges: dict[str, tuple[int, int]]


@dataclass
class _PartialResult:
    """What a worker returns from its slice of the CSV. All numpy
    arrays so the pickle round-trip is cheap. Per-pair state is sparse
    (only the seqs this worker actually touched)."""

    count: np.ndarray
    sum_lat: np.ndarray
    min_lat: np.ndarray
    max_lat: np.ndarray
    late_count: np.ndarray
    hist: np.ndarray
    # pair_id -> (unique seqs, min latency_ns for each) per role. The
    # seq array and the minlat array are positionally aligned; a seq's
    # minlat is the smallest latency_ns across every copy of that role.
    pair_primary_seqs: dict[str, np.ndarray] = field(default_factory=dict)
    pair_primary_minlat: dict[str, np.ndarray] = field(default_factory=dict)
    pair_redundant_seqs: dict[str, np.ndarray] = field(default_factory=dict)
    pair_redundant_minlat: dict[str, np.ndarray] = field(default_factory=dict)
    # pair_id -> (unique seqs, earliest rx for each) seen by this worker
    pair_first_seqs: dict[str, np.ndarray] = field(default_factory=dict)
    pair_first_rxs: dict[str, np.ndarray] = field(default_factory=dict)


def _empty_partial(n_buckets: int) -> _PartialResult:
    return _PartialResult(
        count=np.zeros(n_buckets, dtype=np.int64),
        sum_lat=np.zeros(n_buckets, dtype=np.int64),
        min_lat=np.full(n_buckets, np.iinfo(np.int64).max, dtype=np.int64),
        max_lat=np.full(n_buckets, np.iinfo(np.int64).min, dtype=np.int64),
        late_count=np.zeros(n_buckets, dtype=np.int64),
        hist=np.zeros((n_buckets, HIST_N_BINS), dtype=np.int32),
    )


def _vec_pair_ids(senders: np.ndarray) -> np.ndarray:
    """Vectorized derivation of pair_id from a sender_eui64 array.

    Two input dialects are accepted, depending on which input format the
    caller is iterating over:

    - String EUI-64 (CSV / CSV.gz input): replace chars at positions
      9-10 and 12-13 of ``xx:xx:xx:NN:NN:xx:xx:xx:xx`` with ``00:00``.
    - uint64 EUI-64 (.colbin input): mask out the b3 / b4 mid bytes.
      On a little-endian host with network-byte-order EUI-64 stored as
      a native u64, those mid bytes sit at u64 bits 24–39, so a single
      bitwise AND with ``0xFFFFFF0000FFFFFF`` zeros them.

    Returned dtype mirrors the input dtype (object for strings, uint64
    for binary), and both are valid hashable keys for pandas groupby."""
    if senders.dtype == np.uint64:
        return senders & 0xFFFFFF0000FFFFFF
    s = pd.Series(senders, dtype="string")
    return (s.str.slice(0, 9) + "00:00" + s.str.slice(14)).to_numpy(dtype=object)


def _aggregate_chunk(
    chunk: pd.DataFrame,
    t0_ns: int,
    bucket_ns: int,
    n_buckets: int,
    wcl_ns: int,
    result: _PartialResult,
) -> None:
    """Vectorized aggregator: takes one chunk of the CSV (already a
    DataFrame) and folds it into `result`. No Python-level per-row
    loops. Per-pair state is accumulated into a transient dict of
    column arrays via `pd.DataFrame.groupby` (vectorized) and
    appended onto the result's sparse lists at the end."""
    chunk = chunk[
        chunk["role"].isin(["remote_primary", "remote_redundant", "remote_legacy"])
    ]
    if chunk.empty:
        return

    rx = chunk["rx_gptp_ns"].to_numpy()
    lat = chunk["latency_ns"].to_numpy()
    roles = chunk["role"].astype(str).to_numpy()
    # NOTE: do not .astype(str) on sender_eui64 here. For .colbin input the
    # column dtype is uint64; .astype(str) would yield the *decimal*
    # representation of the u64 (e.g. "17186442064521241224"), and the
    # string branch of _vec_pair_ids would then slice characters 9–13 —
    # which are arbitrary decimal digits, not the b3/b4 mid bytes. That
    # splits primary (mid=0x0000) and redundant (mid=0x0001) into two
    # different pair_ids, breaking the recovered/true_loss derivation.
    # Pass the column as-is and let _vec_pair_ids dispatch on dtype.
    senders = chunk["sender_eui64"].to_numpy()
    seqs = chunk["sequence"].to_numpy(dtype=np.int64)

    # Per-bucket stats use only rows inside the trim window. Per-pair
    # recovered / true_loss tracking below deliberately uses ALL rows:
    # whether a seq's primary / redundant copy arrived is a global
    # property of the capture, not of the trim window. A seq's primary
    # and redundant copies arrive a few ms apart; if the window edge
    # falls between them, dropping the out-of-window copy from per-pair
    # tracking would manufacture a false recovered (or true_loss) at the
    # boundary. Trim-region seqs are still kept off the plot because
    # _derive_recovered_true_loss buckets them by rx and discards bucket
    # indices outside [0, n_buckets).
    bucket_idx = ((rx - t0_ns) // bucket_ns).astype(np.int64)
    in_window = (bucket_idx >= 0) & (bucket_idx < n_buckets)
    if in_window.any():
        bi = bucket_idx[in_window]
        lv = lat[in_window]
        # Per-bucket scatter aggregation
        np.add.at(result.count, bi, 1)
        np.add.at(result.sum_lat, bi, lv)
        np.minimum.at(result.min_lat, bi, lv)
        np.maximum.at(result.max_lat, bi, lv)
        np.add.at(result.late_count, bi, (lv > wcl_ns).astype(np.int64))
        np.add.at(result.hist, (bi, _hist_bin_indices(lv)), 1)

    # Per-pair state (sparse), from all remote rows in the chunk.
    # Compute pair_ids vectorized once per chunk.
    pair_ids = _vec_pair_ids(senders)
    is_primary = (roles == "remote_primary") | (roles == "remote_legacy")
    is_redundant = roles == "remote_redundant"

    # groupby pair_id; for each, compute the per-role (seq -> min
    # latency) maps and the (seq -> earliest rx) pairs for
    # first-arrival tracking.
    df = pd.DataFrame(
        {
            "pair_id": pair_ids,
            "seq": seqs,
            "rx": rx,
            "lat": lat,
            "is_primary": is_primary,
            "is_redundant": is_redundant,
        }
    )
    for pid, sub in df.groupby("pair_id", sort=False):
        # Per-role (seq -> min latency_ns). The Series index (seq) and
        # its values (latency) stay positionally aligned.
        p = sub.loc[sub["is_primary"]].groupby("seq", sort=False)["lat"].min()
        r = sub.loc[sub["is_redundant"]].groupby("seq", sort=False)["lat"].min()
        # Per (seq -> earliest rx) aggregation across all roles.
        agg = sub.groupby("seq", sort=False)["rx"].min()
        first_seqs = agg.index.to_numpy(dtype=np.int64)
        first_rxs = agg.to_numpy(dtype=np.int64)

        result.pair_primary_seqs.setdefault(pid, []).append(
            p.index.to_numpy(dtype=np.int64)
        )
        result.pair_primary_minlat.setdefault(pid, []).append(
            p.to_numpy(dtype=np.int64)
        )
        result.pair_redundant_seqs.setdefault(pid, []).append(
            r.index.to_numpy(dtype=np.int64)
        )
        result.pair_redundant_minlat.setdefault(pid, []).append(
            r.to_numpy(dtype=np.int64)
        )
        result.pair_first_seqs.setdefault(pid, []).append(first_seqs)
        result.pair_first_rxs.setdefault(pid, []).append(first_rxs)


def _finalize_partial(p: _PartialResult) -> _PartialResult:
    """Concatenate the per-chunk lists of sparse arrays into single
    arrays before pickling back to the master."""

    def collapse(d: dict[str, list]) -> dict[str, np.ndarray]:
        return {
            pid: np.concatenate(arrs) if arrs else np.empty(0, dtype=np.int64)
            for pid, arrs in d.items()
        }

    p.pair_primary_seqs = collapse(p.pair_primary_seqs)
    p.pair_primary_minlat = collapse(p.pair_primary_minlat)
    p.pair_redundant_seqs = collapse(p.pair_redundant_seqs)
    p.pair_redundant_minlat = collapse(p.pair_redundant_minlat)
    p.pair_first_seqs = collapse(p.pair_first_seqs)
    p.pair_first_rxs = collapse(p.pair_first_rxs)
    return p


def _scan_metadata(path: Path, chunksize: int) -> _ScanMeta:
    """Pass 1: stream once to find time range AND per-pair seq range.
    These dimensions size the per-pair bitmaps in pass 2."""
    t0 = None
    t1 = None
    total = 0
    pair_seq_min: dict[str, int] = {}
    pair_seq_max: dict[str, int] = {}
    cols = ["rx_gptp_ns", "sender_eui64", "sequence", "role"]
    dtypes = {k: v for k, v in CSV_DTYPES.items() if k in cols}
    reader = pd.read_csv(
        path,
        dtype=dtypes,
        usecols=cols,
        chunksize=chunksize,
        on_bad_lines="skip",
    )
    for chunk in _iter_chunks_tolerant(reader, label="scan"):
        chunk = chunk[
            chunk["role"].isin(["remote_primary", "remote_redundant", "remote_legacy"])
        ]
        if chunk.empty:
            continue
        rx = chunk["rx_gptp_ns"].to_numpy()
        cmin, cmax = int(rx.min()), int(rx.max())
        t0 = cmin if t0 is None else min(t0, cmin)
        t1 = cmax if t1 is None else max(t1, cmax)
        total += len(rx)
        pair_ids = _vec_pair_ids(chunk["sender_eui64"].astype(str).to_numpy())
        seqs = chunk["sequence"].to_numpy(dtype=np.int64)
        agg = (
            pd.DataFrame({"pid": pair_ids, "seq": seqs})
            .groupby("pid", sort=False)["seq"]
            .agg(["min", "max"])
        )
        for pid, row in agg.iterrows():
            mn, mx = int(row["min"]), int(row["max"])
            pair_seq_min[pid] = (
                mn if pid not in pair_seq_min else min(pair_seq_min[pid], mn)
            )
            pair_seq_max[pid] = (
                mx if pid not in pair_seq_max else max(pair_seq_max[pid], mx)
            )
    if t0 is None or t1 is None:
        raise ValueError(f"no remote rows in {path}")
    return _ScanMeta(
        t0_ns=t0,
        t1_ns=t1,
        total_rows=total,
        pair_seq_ranges={
            pid: (pair_seq_min[pid], pair_seq_max[pid]) for pid in pair_seq_min
        },
    )


def _is_gzipped(path: Path) -> bool:
    """True if the path looks like a gzipped CSV. Suffix-based — pandas
    uses the same heuristic when `compression="infer"`."""
    return path.suffix.lower() == ".gz"


def _is_colbin(path: Path) -> bool:
    """True if the path looks like a .colbin file (statusbar packed binary)."""
    return path.suffix.lower() == ".colbin"


def _parse_colbin_header(path: Path):
    """Read and validate the .colbin header. Returns
    (header_total, row_size, structured_dtype, committed_rows, names)."""
    with open(path, "rb") as f:
        preamble = f.read(COLBIN_HEADER_PREAMBLE)
    if len(preamble) < COLBIN_HEADER_PREAMBLE or preamble[:8] != COLBIN_MAGIC:
        raise ValueError(f"{path}: not a colbin file (bad magic)")
    version = int.from_bytes(preamble[8:12], "little")
    if version != COLBIN_VERSION:
        raise ValueError(f"{path}: unsupported colbin version {version}")
    endian = int.from_bytes(preamble[12:16], "little")
    if endian != COLBIN_ENDIAN_MARKER:
        raise ValueError(
            f"{path}: endian marker mismatch (file from different host endianness)"
        )
    header_total = int.from_bytes(preamble[16:20], "little")
    row_size = int.from_bytes(preamble[20:24], "little")
    committed = int.from_bytes(preamble[24:32], "little")
    schema_len = int.from_bytes(preamble[32:36], "little")
    with open(path, "rb") as f:
        f.seek(COLBIN_HEADER_PREAMBLE)
        schema_text = f.read(schema_len).decode("ascii")
    fields = []
    for col in schema_text.split(","):
        name, _, tname = col.partition(":")
        if tname not in COLBIN_TYPE_TO_DTYPE:
            raise ValueError(f"{path}: unknown colbin type '{tname}' in schema")
        fields.append((name, COLBIN_TYPE_TO_DTYPE[tname]))
    dtype = np.dtype(fields, align=True)
    if dtype.itemsize != row_size:
        raise ValueError(
            f"{path}: numpy dtype size {dtype.itemsize} != on-disk row_size {row_size}"
        )
    names = [name for (name, _) in fields]
    return header_total, row_size, dtype, committed, names


def _colbin_chunk_to_df(view: np.ndarray) -> pd.DataFrame:
    """Build a pandas DataFrame from a structured-ndarray slice of a
    colbin memmap. The role column (u8) is mapped to string so the
    same `role.isin([...])` filter used for CSV input keeps working."""
    df = pd.DataFrame(view)
    if "role" in df.columns:
        role_u8 = df["role"].to_numpy()
        clamped = np.clip(role_u8, 0, len(COLBIN_ROLE_NAMES) - 1)
        out = COLBIN_ROLE_NAMES[clamped].copy()
        out[role_u8 >= len(COLBIN_ROLE_NAMES)] = "unknown"
        df["role"] = out
    return df


def _iter_colbin_chunks(path: Path, chunksize: int):
    """Yield pandas DataFrames from a .colbin file, one chunksize-row
    slice at a time. Uses np.memmap so pages are loaded lazily."""
    header_total, _, dtype, committed, _ = _parse_colbin_header(path)
    if committed == 0:
        return
    mm = np.memmap(path, dtype=dtype, mode="r", offset=header_total, shape=(committed,))
    for start in range(0, committed, max(1, chunksize)):
        end = min(start + chunksize, committed)
        yield _colbin_chunk_to_df(mm[start:end])


def _scan_metadata_colbin(path: Path) -> _ScanMeta:
    """Pass-1 equivalent for .colbin: read the time range and per-pair
    sequence ranges directly from the memmap'd file."""
    header_total, _, dtype, committed, _ = _parse_colbin_header(path)
    if committed == 0:
        raise ValueError(f"no rows in {path}")
    mm = np.memmap(path, dtype=dtype, mode="r", offset=header_total, shape=(committed,))
    role_u8 = mm["role"]
    # 3 = RemotePrimary, 4 = RemoteRedundant, 5 = RemoteLegacy.
    keep = (role_u8 >= 3) & (role_u8 <= 5)
    if not keep.any():
        raise ValueError(f"no remote rows in {path}")
    rx = mm["rx_gptp_ns"][keep]
    t0 = int(rx.min())
    t1 = int(rx.max())
    total = int(keep.sum())
    sender = mm["sender_eui64"][keep]
    seq = mm["sequence"][keep].astype(np.int64)
    # Zero the redundancy mid bytes (b3, b4 in network order). On a
    # little-endian host these are u64 bits 24–39.
    pid = sender & 0xFFFFFF0000FFFFFF
    agg = (
        pd.DataFrame({"pid": pid, "seq": seq})
        .groupby("pid", sort=False)["seq"]
        .agg(["min", "max"])
    )
    pair_seq_ranges = {
        p: (int(row["min"]), int(row["max"])) for p, row in agg.iterrows()
    }
    return _ScanMeta(
        t0_ns=t0, t1_ns=t1, total_rows=total, pair_seq_ranges=pair_seq_ranges
    )


def _process_streaming_colbin(
    path: Path,
    t0_ns: int,
    bucket_ns: int,
    n_buckets: int,
    wcl_ns: int,
    chunksize: int,
) -> _PartialResult:
    """Pass-2 equivalent for .colbin. Same shape as `_process_streaming`
    but consumes `_iter_colbin_chunks` instead of pandas' CSV reader."""
    result = _empty_partial(n_buckets)
    for chunk in _iter_colbin_chunks(path, chunksize):
        _aggregate_chunk(chunk, t0_ns, bucket_ns, n_buckets, wcl_ns, result)
    return _finalize_partial(result)


def _process_streaming(
    path: Path,
    t0_ns: int,
    bucket_ns: int,
    n_buckets: int,
    wcl_ns: int,
    chunksize: int,
) -> _PartialResult:
    """Single-process streaming aggregation. Used for gzipped inputs
    because gzip is a stream format that can't be seeked to arbitrary
    row boundaries, so the byte-range parallel path doesn't apply.
    Pandas auto-detects compression from the suffix and decompresses
    on the fly without materializing the whole file."""
    result = _empty_partial(n_buckets)
    reader = pd.read_csv(
        path,
        dtype=CSV_DTYPES,
        chunksize=chunksize,
        on_bad_lines="skip",
    )
    for chunk in _iter_chunks_tolerant(reader, label=f"stream {path.name}"):
        _aggregate_chunk(chunk, t0_ns, bucket_ns, n_buckets, wcl_ns, result)
    return _finalize_partial(result)


def _file_byte_ranges(path: Path, n_workers: int) -> list[tuple[int, int]]:
    """Split the file into byte ranges aligned to CSV row boundaries.
    The first range starts right after the header line; subsequent
    boundaries are bumped forward to the next newline."""
    size = path.stat().st_size
    with open(path, "rb") as f:
        f.readline()  # skip header line
        body_start = f.tell()
    body_size = size - body_start
    if n_workers <= 1 or body_size == 0:
        return [(body_start, size)]
    raw = [body_start + (body_size * i) // n_workers for i in range(n_workers + 1)]
    aligned = [body_start]
    with open(path, "rb") as f:
        for b in raw[1:-1]:
            f.seek(b)
            f.readline()
            aligned.append(f.tell())
    aligned.append(size)
    return list(zip(aligned[:-1], aligned[1:]))


def _process_byte_range(args: tuple) -> _PartialResult:
    """Worker entry point. Reads the assigned byte range, parses it
    as headerless CSV with our known column names, and folds it into
    a `_PartialResult` via the vectorized aggregator."""
    path_str, start, end, t0_ns, bucket_ns, n_buckets, wcl_ns, chunksize = args
    path = Path(path_str)

    with open(path, "rb") as f:
        f.seek(start)
        data = f.read(end - start)

    result = _empty_partial(n_buckets)
    reader = pd.read_csv(
        io.BytesIO(data),
        header=None,
        names=CSV_COLUMNS,
        dtype=CSV_DTYPES,
        chunksize=chunksize,
        on_bad_lines="skip",
    )
    for chunk in _iter_chunks_tolerant(reader, label=f"byte-range {start}-{end}"):
        _aggregate_chunk(chunk, t0_ns, bucket_ns, n_buckets, wcl_ns, result)
    return _finalize_partial(result)


def _merge_partials(partials: list[_PartialResult]) -> _PartialResult:
    """Reduce per-worker partials into a single result by elementwise
    add/min/max on bucket arrays and list-concat on sparse pair lists."""
    if not partials:
        raise ValueError("no partials to merge")
    out = partials[0]
    for p in partials[1:]:
        out.count += p.count
        out.sum_lat += p.sum_lat
        np.minimum(out.min_lat, p.min_lat, out=out.min_lat)
        np.maximum(out.max_lat, p.max_lat, out=out.max_lat)
        out.late_count += p.late_count
        out.hist += p.hist
        for src, dst in [
            (p.pair_primary_seqs, out.pair_primary_seqs),
            (p.pair_primary_minlat, out.pair_primary_minlat),
            (p.pair_redundant_seqs, out.pair_redundant_seqs),
            (p.pair_redundant_minlat, out.pair_redundant_minlat),
        ]:
            for pid, arr in src.items():
                if pid in dst:
                    dst[pid] = np.concatenate([dst[pid], arr])
                else:
                    dst[pid] = arr
        # First-arrival rx: concatenate; deduplication happens in finalize.
        for pid in p.pair_first_seqs:
            if pid in out.pair_first_seqs:
                out.pair_first_seqs[pid] = np.concatenate(
                    [out.pair_first_seqs[pid], p.pair_first_seqs[pid]]
                )
                out.pair_first_rxs[pid] = np.concatenate(
                    [out.pair_first_rxs[pid], p.pair_first_rxs[pid]]
                )
            else:
                out.pair_first_seqs[pid] = p.pair_first_seqs[pid]
                out.pair_first_rxs[pid] = p.pair_first_rxs[pid]
    return out


def _discontinuity_unseen_mask(rx_at_seq: np.ndarray, seen: np.ndarray) -> np.ndarray:
    """Mask (over a sender's per-seq span) of never-seen sequences that
    belong to a *sequence discontinuity* — a talker stream restart, seq
    reset, or wrap — rather than a genuine reception gap.

    For one well-behaved stream, receive time is monotonic non-decreasing
    with sequence number, so a maximal run of never-seen sequences bounded
    by two seen packets is a real outage only when the bounding packets are
    in time order (rx of the seq *after* the hole > rx of the seq
    *before* it). When they are out of order — the higher-seq bounding
    packet arrived no later than the lower-seq one — the missing range is
    an artifact of a non-monotonic sequence stream (e.g. a restart whose
    new sequence numbers overlap the old), not lost audio. Counting it
    yields phantom drops spanning much of the capture, often with a
    *negative* outage duration. Such runs are masked here so they don't
    inflate true_loss or surface as bogus drop bursts.

    Edge runs (no bounding seen packet on one side, i.e. the capture
    started or ended mid-hole) cannot be judged and are left unmasked, so
    genuine capture-edge loss is still reported (as `bounded=False`)."""
    span = rx_at_seq.size
    mask = np.zeros(span, dtype=bool)
    unseen = ~seen
    if span == 0 or not unseen.any():
        return mask
    padded = np.concatenate(([0], unseen.astype(np.int8), [0]))
    edges = np.diff(padded)
    starts = np.where(edges == 1)[0]
    ends = np.where(edges == -1)[0] - 1
    for a, b in zip(starts, ends):
        before, after = int(a) - 1, int(b) + 1
        if before < 0 or after >= span:
            continue  # edge run: no bounding packet to test time order
        if rx_at_seq[after] <= rx_at_seq[before]:
            mask[int(a) : int(b) + 1] = True
    return mask


def _derive_recovered_true_loss(
    merged: _PartialResult,
    scan: _ScanMeta,
    bucket_ns: int,
    n_buckets: int,
    wcl_ns: int,
) -> tuple[np.ndarray, np.ndarray]:
    """Given the merged sparse per-pair state, materialize per-pair
    min-latency arrays (per role) and earliest-rx-per-seq arrays, then
    derive recovered_count and true_loss_count per bucket.

    A sequence is delivered iff at least one copy arrived within WCL:

        primary_in_time   = min primary/legacy latency_ns <= wcl_ns
        redundant_in_time = min redundant      latency_ns <= wcl_ns

    (each is False when no copy of that role arrived). Then:

        recovered = ¬primary_in_time ∧ redundant_in_time
        true_loss = ¬primary_in_time ∧ ¬redundant_in_time

    true_loss therefore covers both genuine gaps (no copy at all) and
    sequences where every copy arrived but all of them were late.

    rx_at_seq for never-seen seqs is interpolated from neighbors so the
    true_loss markers land on the right time bucket."""
    recovered_count = np.zeros(n_buckets, dtype=np.int64)
    true_loss_count = np.zeros(n_buckets, dtype=np.int64)
    int64_max = np.iinfo(np.int64).max

    for pid, (min_seq, max_seq) in scan.pair_seq_ranges.items():
        span = max_seq - min_seq + 1
        primary_minlat = np.full(span, int64_max, dtype=np.int64)
        redundant_minlat = np.full(span, int64_max, dtype=np.int64)
        rx_at_seq = np.full(span, int64_max, dtype=np.int64)

        if pid in merged.pair_primary_seqs:
            idx = merged.pair_primary_seqs[pid] - min_seq
            in_range = (idx >= 0) & (idx < span)
            # Scatter-min: a seq may appear in multiple workers / chunks;
            # keep the smallest latency seen for it.
            np.minimum.at(
                primary_minlat,
                idx[in_range],
                merged.pair_primary_minlat[pid][in_range],
            )
        if pid in merged.pair_redundant_seqs:
            idx = merged.pair_redundant_seqs[pid] - min_seq
            in_range = (idx >= 0) & (idx < span)
            np.minimum.at(
                redundant_minlat,
                idx[in_range],
                merged.pair_redundant_minlat[pid][in_range],
            )
        if pid in merged.pair_first_seqs:
            idx = merged.pair_first_seqs[pid] - min_seq
            in_range = (idx >= 0) & (idx < span)
            idx = idx[in_range]
            rxs = merged.pair_first_rxs[pid][in_range]
            np.minimum.at(rx_at_seq, idx, rxs)

        # A seq with no real rx was never seen at all.
        seen = rx_at_seq != int64_max
        if not seen.any():
            continue

        # Mask never-seen runs that are sequence discontinuities (talker
        # restart / reset), not real loss — computed from the raw rx times
        # before the interpolation below overwrites the unseen entries.
        discontinuity = _discontinuity_unseen_mask(rx_at_seq, seen)

        primary_in_time = primary_minlat <= wcl_ns
        redundant_in_time = redundant_minlat <= wcl_ns

        # Interpolate rx for never-seen seqs so true_loss markers land on
        # the right time bucket.
        seen_idx = np.where(seen)[0]
        seen_rxs = rx_at_seq[seen_idx].astype(np.float64)
        unseen_idx = np.where(~seen)[0]
        if unseen_idx.size > 0 and seen_idx.size >= 1:
            rx_at_seq[unseen_idx] = np.interp(
                unseen_idx.astype(np.float64),
                seen_idx.astype(np.float64),
                seen_rxs,
            ).astype(np.int64)

        recovered = ~primary_in_time & redundant_in_time
        true_loss = ~primary_in_time & ~redundant_in_time & ~discontinuity
        for mask, target in (
            (recovered, recovered_count),
            (true_loss, true_loss_count),
        ):
            if not mask.any():
                continue
            bidx = ((rx_at_seq[mask] - scan.t0_ns) // bucket_ns).astype(np.int64)
            valid = (bidx >= 0) & (bidx < n_buckets)
            np.add.at(target, bidx[valid], 1)

    return recovered_count, true_loss_count


def summarize_csv(
    path: Path,
    out_path: Path,
    plot_width: int = DEFAULT_PLOT_WIDTH,
    wcl_ms: float = 60.0,
    chunksize: int = DEFAULT_CHUNKSIZE,
    min_bucket_ms: float = 1.0,
    verbose: bool = False,
    n_workers: int | None = None,
    trim_seconds: float = 5.0,
) -> SummaryMeta:
    """Stream the CSV, produce a per-bucket summary parquet with a
    JSON sidecar containing the bucket metadata.

    With n_workers > 1, the file is split into byte-aligned ranges and
    each worker process aggregates its slice in parallel; the master
    then reduces the partials. n_workers=None defaults to all CPUs.

    `trim_seconds` discards that many seconds from each end of the
    captured time range — useful for steady-state long-duration tests
    where the first and last few seconds contain warm-up or shutdown
    transients. If the trim would leave a non-positive window, the
    trim is skipped with a warning."""
    if n_workers is None:
        n_workers = max(1, os.cpu_count() or 1)

    # Pass 1: scan time range and per-pair sequence ranges.
    if verbose:
        print(f"[summarize] pass 1: scanning {path}…", file=sys.stderr)
    scan = (
        _scan_metadata_colbin(path)
        if _is_colbin(path)
        else _scan_metadata(path, chunksize)
    )

    # Trim warm-up / cool-down from each end of the capture window.
    trim_ns = int(max(0.0, trim_seconds) * 1_000_000_000)
    window_t0 = scan.t0_ns + trim_ns
    window_t1 = scan.t1_ns - trim_ns
    if window_t1 - window_t0 <= 0:
        if trim_seconds > 0 and verbose:
            print(
                f"[summarize] warning: trim_seconds={trim_seconds:g} >= half the "
                f"captured span ({(scan.t1_ns - scan.t0_ns) / 1e9:.3f}s); "
                f"skipping trim.",
                file=sys.stderr,
            )
        window_t0 = scan.t0_ns
        window_t1 = scan.t1_ns
    span_ns = max(1, window_t1 - window_t0)
    bucket_ns = max(int(min_bucket_ms * 1_000_000), span_ns // max(1, plot_width))
    n_buckets = int(span_ns // bucket_ns) + 1
    wcl_ns = int(wcl_ms * 1_000_000)

    if verbose:
        trim_note = (
            f" (trimmed {trim_seconds:g}s from each end of "
            f"{(scan.t1_ns - scan.t0_ns) / 1e9:.1f}s capture)"
            if window_t0 != scan.t0_ns
            else ""
        )
        print(
            f"[summarize] span={span_ns / 1e9:.1f}s{trim_note} rows={scan.total_rows} "
            f"buckets={n_buckets} bucket_ms={bucket_ns / 1e6:.1f} workers={n_workers}",
            file=sys.stderr,
        )

    # Pass 2: aggregate. Byte-range parallelism only works on plain
    # CSV; gzipped inputs must stream single-process because gzip is
    # not seekable to row boundaries. colbin uses np.memmap, so the
    # streaming path is already as fast as it gets.
    if _is_colbin(path):
        if verbose:
            print("[summarize] pass 2: colbin → mmap stream", file=sys.stderr)
        merged = _process_streaming_colbin(
            path,
            window_t0,
            bucket_ns,
            n_buckets,
            wcl_ns,
            chunksize,
        )
    elif _is_gzipped(path):
        if verbose:
            print(
                "[summarize] pass 2: gzip → single-stream (no byte-range parallelism)",
                file=sys.stderr,
            )
        merged = _process_streaming(
            path,
            window_t0,
            bucket_ns,
            n_buckets,
            wcl_ns,
            chunksize,
        )
    else:
        ranges = _file_byte_ranges(path, n_workers)
        common_args = (window_t0, bucket_ns, n_buckets, wcl_ns, chunksize)
        worker_args = [(str(path), start, end) + common_args for (start, end) in ranges]
        if verbose:
            print(
                f"[summarize] pass 2: {len(ranges)} byte-range workers", file=sys.stderr
            )
        if n_workers > 1 and len(ranges) > 1:
            with mp.get_context("spawn").Pool(n_workers) as pool:
                partials = pool.map(_process_byte_range, worker_args)
        else:
            partials = [_process_byte_range(worker_args[0])]
        merged = _merge_partials(partials)

    # Per-pair recovered / true_loss is classified over the full
    # captured seq range; the trim window is applied per-bucket inside
    # _derive_recovered_true_loss (out-of-window seqs get a bucket index
    # outside [0, n_buckets) and are dropped). pair_first_seqs spans the
    # whole capture, so its min/max is the full per-pair seq range.
    full_pair_seq_ranges: dict[str, tuple[int, int]] = {}
    for pid, seqs in merged.pair_first_seqs.items():
        if seqs.size > 0:
            full_pair_seq_ranges[pid] = (int(seqs.min()), int(seqs.max()))
    windowed_scan = _ScanMeta(
        t0_ns=window_t0,
        t1_ns=window_t1,
        total_rows=scan.total_rows,
        pair_seq_ranges=full_pair_seq_ranges,
    )

    if verbose:
        print("[summarize] deriving recovered / true_loss…", file=sys.stderr)
    recovered_count, true_loss_count = _derive_recovered_true_loss(
        merged,
        windowed_scan,
        bucket_ns,
        n_buckets,
        wcl_ns,
    )

    # Sentinel cleanup: empty buckets should not carry int64 max/min.
    empty = merged.count == 0
    min_lat = np.where(empty, 0, merged.min_lat)
    max_lat = np.where(empty, 0, merged.max_lat)

    out_df = pd.DataFrame(
        {
            "bucket_idx": np.arange(n_buckets, dtype=np.int64),
            "t_ns": (window_t0 + np.arange(n_buckets, dtype=np.int64) * bucket_ns),
            "count": merged.count,
            "min_lat_ns": min_lat,
            "max_lat_ns": max_lat,
            "sum_lat_ns": merged.sum_lat,
            "late_count": merged.late_count,
            "recovered_count": recovered_count,
            "true_loss_count": true_loss_count,
        }
    )
    for i in range(HIST_N_BINS):
        out_df[f"hist_{i}"] = merged.hist[:, i]
    out_df.to_parquet(out_path)

    meta = SummaryMeta(
        t0_ns=window_t0,
        t1_ns=window_t1,
        bucket_ns=bucket_ns,
        n_buckets=n_buckets,
        wcl_ns=wcl_ns,
        total_rows=scan.total_rows,
        source_csv=str(path),
    )
    Path(str(out_path) + ".json").write_text(json.dumps(meta.to_dict(), indent=2))
    return meta


def load_summary(path: Path) -> tuple[pd.DataFrame, SummaryMeta, np.ndarray]:
    """Load a summary parquet plus its JSON sidecar. Returns the bucket
    DataFrame, the metadata, and a (n_buckets, HIST_N_BINS) int array
    extracted from the hist_* columns."""
    df = pd.read_parquet(path)
    meta = SummaryMeta.from_dict(json.loads(Path(str(path) + ".json").read_text()))
    hist_cols = [f"hist_{i}" for i in range(HIST_N_BINS)]
    hist = df[hist_cols].to_numpy(dtype=np.int64)
    # Drop the hist_* columns from the returned df so callers don't
    # accidentally iterate them as bucket fields.
    df = df.drop(columns=hist_cols)
    return df, meta, hist


# ----------------------------------------------------------------------
# Subcommands
# ----------------------------------------------------------------------


def cmd_summarize(args: argparse.Namespace) -> int:
    meta = summarize_csv(
        path=Path(args.input),
        out_path=Path(args.output),
        plot_width=args.plot_width,
        wcl_ms=args.worst_case_latency_ms,
        chunksize=args.chunksize,
        verbose=True,
        n_workers=args.workers,
        trim_seconds=args.trim_seconds,
    )
    print(
        f"wrote {args.output} ({meta.n_buckets} buckets, "
        f"bucket={meta.bucket_ns / 1e6:.2f}ms, "
        f"span={(meta.t1_ns - meta.t0_ns) / 1e9:.1f}s)"
    )
    return 0


@dataclass
class _SharedRanges:
    """Y-axis caps shared across devices for visual comparability,
    plus the x-axis floor for the histogram panel."""

    latency_y_max_ms: float
    latency_data_top_ms: float
    pnn_y_max_ms: float
    hist_x_min_ms: float
    hist_x_max_ms: float


def _format_stats_header_lines(
    df: pd.DataFrame,
    meta,
    hist: np.ndarray,
    label: str,
    percentiles: list[float],
) -> list[str]:
    """Build the text block for one device in the stats header. Always
    shows late / recovered / true_loss counters (even when zero) so a
    clean run is visibly clean rather than the counters silently
    omitted from the legend. Percentile-based required-buffer figures
    (the Tier 1 #1 metric — buffer = pX latency − min latency) follow."""
    cnt = df["count"].to_numpy()
    nonempty = cnt > 0
    lines: list[str] = [label, ""]
    if not nonempty.any():
        lines.append("  (no buckets in window)")
        return lines

    count = int(cnt.sum())
    min_ms = float(df["min_lat_ns"].to_numpy()[nonempty].min()) / 1e6
    max_ms = float(df["max_lat_ns"].to_numpy()[nonempty].max()) / 1e6
    mean_ms = float(df["sum_lat_ns"].sum()) / max(count, 1) / 1e6
    late = int(df["late_count"].sum())
    recov = int(df["recovered_count"].sum())
    loss = int(df["true_loss_count"].sum())
    window_s = (meta.t1_ns - meta.t0_ns) / 1e9
    wcl_ms = meta.wcl_ns / 1e6

    lines.append(f"  packets     {count:>12,}")
    lines.append(f"  min / mean  {min_ms:>7.3f} / {mean_ms:>7.3f} ms")
    lines.append(f"  max         {max_ms:>7.3f} ms   (WCL {wcl_ms:.3f} ms)")
    lines.append(f"  late {late:<5}  recovered {recov:<5}  true_loss {loss}")

    # Required receive buffer at percentile = pX - min. The histograms in
    # the parquet are per-bucket; sum to get an overall distribution.
    if percentiles:
        overall_hist = hist.sum(axis=0).reshape(1, -1)
        pct_ns = _bucket_percentiles_interp(overall_hist, percentiles)[0]
        lines.append("")
        lines.append("  required receive buffer (= pX latency − min):")
        for p, ns in zip(percentiles, pct_ns):
            if np.isnan(ns):
                continue
            ms = ns / 1e6
            buf = ms - min_ms
            lines.append(f"    @ p{p:<7g} {buf:>7.3f} ms   (pX {ms:.3f} ms)")

    lines.append("")
    lines.append(f"  window      {window_s:>7.1f} s")
    return lines


def _draw_stats_header_panel(
    ax,
    summaries: list[tuple[pd.DataFrame, object, np.ndarray, str]],
    percentiles: list[float],
) -> None:
    """Render a single-row stats header at the top of the figure. The
    panel itself draws no data — it carries one text column per device
    with the count / latency-floor / late+lost+recovered counters and
    the percentile-based required-buffer figures. Sits above the data
    panels via the figure's GridSpec; height controlled by
    ``--stats-header-inches`` on cmd_plot."""
    ax.axis("off")
    n_dev = len(summaries)
    if n_dev == 0:
        return
    col_width = 1.0 / n_dev
    for idx, (df, meta, hist, label) in enumerate(summaries):
        lines = _format_stats_header_lines(df, meta, hist, label, percentiles)
        ax.text(
            idx * col_width + 0.01,
            0.98,
            "\n".join(lines),
            transform=ax.transAxes,
            va="top",
            ha="left",
            family="monospace",
            fontsize=9,
        )
    # Thin divider line at the bottom of the header so the eye sees
    # where the stats panel ends and the data panels begin.
    ax.axhline(0.0, color="0.7", linewidth=0.5)


def _draw_latency_panel(ax, df, meta, hist, label, ranges: _SharedRanges):
    """Latency vs wall time panel: min/max band + mean line + WCL line
    + event markers (late, recovered, true_loss). Y-axis shared across
    devices via `ranges.latency_y_max_ms`."""
    t_s = (df["t_ns"].to_numpy() - meta.t0_ns) / 1e9
    wcl_ms = meta.wcl_ns / 1e6
    mask = df["count"].to_numpy() > 0
    min_ms = df["min_lat_ns"].to_numpy() / 1e6
    max_ms = df["max_lat_ns"].to_numpy() / 1e6
    cnt = df["count"].to_numpy()
    sum_ns = df["sum_lat_ns"].to_numpy()
    mean_ms = np.where(cnt > 0, sum_ns / np.maximum(cnt, 1), 0) / 1e6

    # Per-device actuals — what gets printed in the stats box.
    if mask.any():
        actual_min_ms = float(min_ms[mask].min())
        actual_max_ms = float(max_ms[mask].max())
        actual_mean_ms = float(sum_ns.sum()) / max(int(cnt.sum()), 1) / 1e6
    else:
        actual_min_ms = actual_max_ms = actual_mean_ms = float("nan")

    ax.fill_between(
        t_s[mask],
        min_ms[mask],
        max_ms[mask],
        alpha=0.25,
        color="steelblue",
        linewidth=0,
        step="mid",
        label="min–max",
    )
    ax.plot(t_s[mask], mean_ms[mask], color="darkblue", linewidth=0.5, label="mean")
    ax.axhline(
        wcl_ms,
        color="red",
        linestyle="--",
        alpha=0.5,
        linewidth=1,
        label=f"WCL={wcl_ms:.0f}ms",
    )

    y_top = ranges.latency_y_max_ms

    # Event markers (late / recovered / true-loss) are *categorical*: a
    # marker means "this bucket contained >=1 such event". Their vertical
    # position carries no latency meaning. Drawing them on the latency
    # scale — e.g. just above the WCL line — invites the reader to misread
    # marker height as measured latency. So they are confined to a shaded
    # lane strictly above the latency-data region (min-max band, mean,
    # WCL line all sit at or below `latency_data_top_ms`), with a divider
    # line and a legend entry spelling out that the lane is not latency.
    band_lo = ranges.latency_data_top_ms
    band_h = y_top - band_lo
    ax.axhspan(
        band_lo,
        y_top,
        facecolor="0.92",
        edgecolor="none",
        zorder=0,
        label="event-marker lane (height ≠ latency)",
    )
    ax.axhline(band_lo, color="0.6", linewidth=0.8, zorder=1)
    y_late = band_lo + band_h * (1 / 6)
    y_recovered = band_lo + band_h * (3 / 6)
    y_loss = band_lo + band_h * (5 / 6)

    late_mask = df["late_count"].to_numpy() > 0
    recov_mask = df["recovered_count"].to_numpy() > 0
    loss_mask = df["true_loss_count"].to_numpy() > 0
    n_late = int(df["late_count"].sum())
    n_recovered = int(df["recovered_count"].sum())
    n_true_loss = int(df["true_loss_count"].sum())
    if late_mask.any():
        ax.scatter(
            t_s[late_mask],
            np.full(late_mask.sum(), y_late),
            c="darkred",
            s=8,
            marker="^",
            zorder=3,
            label=f"late ({n_late})",
        )
    if recov_mask.any():
        ax.scatter(
            t_s[recov_mask],
            np.full(recov_mask.sum(), y_recovered),
            c="tab:orange",
            s=10,
            marker="o",
            edgecolors="black",
            linewidths=0.3,
            zorder=4,
            label=f"recovered ({n_recovered})",
        )
    if loss_mask.any():
        ax.scatter(
            t_s[loss_mask],
            np.full(loss_mask.sum(), y_loss),
            c="red",
            s=14,
            marker="x",
            zorder=5,
            label=f"true loss ({n_true_loss})",
        )

    ax.set_ylim(top=y_top)
    ax.set_xlabel("session time (s)")
    ax.set_ylabel("latency (ms)")
    start_str = _tai_ns_to_pdt_str(int(meta.t0_ns))
    ax.set_title(f"{label}: latency vs wall time  (start: {start_str})")
    ax.legend(loc="upper right", fontsize=7, ncol=3)

    # Stats annotation, top-left so it doesn't fight the legend.
    stats_text = (
        f"min:  {actual_min_ms:.3f} ms\n"
        f"mean: {actual_mean_ms:.3f} ms\n"
        f"max:  {actual_max_ms:.3f} ms\n"
        f"WCL:  {wcl_ms:.3f} ms"
    )
    ax.text(
        0.01,
        0.98,
        stats_text,
        transform=ax.transAxes,
        verticalalignment="top",
        horizontalalignment="left",
        fontsize=8,
        family="monospace",
        bbox=dict(
            boxstyle="round,pad=0.25", facecolor="white", alpha=0.85, edgecolor="0.7"
        ),
    )


def _draw_pnn_panel(ax, df, meta, hist, label, ranges: _SharedRanges):
    """Rolling pNN panel: p50/p95/p99 derived per-bucket from the
    per-bucket histogram. Y-axis shared across devices."""
    t_s = (df["t_ns"].to_numpy() - meta.t0_ns) / 1e9
    percentiles = [50.0, 95.0, 99.0]
    pNN_ns = _bucket_percentiles(hist, percentiles)
    pNN_ms = pNN_ns / 1e6
    mask = df["count"].to_numpy() > 0
    colors = ["tab:blue", "tab:orange", "tab:red"]
    for j, (p, color) in enumerate(zip(percentiles, colors)):
        ax.plot(
            t_s[mask], pNN_ms[mask, j], label=f"p{int(p)}", color=color, linewidth=0.7
        )
    ax.axhline(meta.wcl_ns / 1e6, color="red", linestyle="--", alpha=0.4, linewidth=1)
    ax.set_ylim(top=ranges.pnn_y_max_ms)
    ax.set_xlabel("session time (s)")
    ax.set_ylabel("latency (ms)")
    ax.set_title(f"{label}: rolling pNN (bucket={meta.bucket_ns / 1e6:.1f}ms)")
    ax.legend(loc="upper right", fontsize=8)


def _draw_histogram_panel(ax, df, meta, hist, label, ranges: _SharedRanges):
    """Global latency distribution: sum of per-bucket histograms,
    plotted with linear x-axis (clipped to `ranges.hist_x_max_ms`) and
    log y-axis. The bins underneath are log-spaced (`HIST_EDGES_NS`),
    so bar widths reflect the linear width of each log bin — wider on
    the right than on the left. Counts (bar heights) are unaffected;
    use the log-x mode if you need a tail-aware density view."""
    totals = hist.sum(axis=0)
    log_counts = totals[1 : HIST_N_LOG_BINS + 1]
    neg = int(totals[HIST_NEG_BIN])
    pos = int(totals[HIST_POS_OVERFLOW_BIN])

    centers_ms = HIST_BIN_CENTERS_NS / 1e6
    widths_ms = (HIST_EDGES_NS[1:] - HIST_EDGES_NS[:-1]) / 1e6
    ax.bar(
        centers_ms,
        log_counts,
        width=widths_ms,
        align="center",
        color="steelblue",
        edgecolor="none",
    )
    ax.set_xscale("linear")
    ax.set_yscale("log")
    ax.set_xlim(0.0, ranges.hist_x_max_ms)

    ax.axvline(
        meta.wcl_ns / 1e6,
        color="red",
        linestyle="--",
        alpha=0.4,
        linewidth=1,
        label=f"WCL={meta.wcl_ns / 1e6:.0f}ms",
    )

    overflow_note = []
    if neg > 0:
        overflow_note.append(f"≤0 ns: {neg}")
    if pos > 0:
        overflow_note.append(f"≥{HIST_LOG_HIGH_NS / 1e9:.0f}s: {pos}")
    title = f"{label}: rx latency distribution"
    if overflow_note:
        title += "  (" + ", ".join(overflow_note) + ")"
    ax.set_title(title)
    ax.set_xlabel("latency (ms)")
    ax.set_ylabel("count (log)")
    ax.legend(loc="upper right", fontsize=8)


def _compute_shared_ranges(summaries, hist_min_ms_floor: float) -> _SharedRanges:
    """Compute axis caps shared across all devices so panels of the
    same kind are visually comparable. The latency y-cap leaves
    headroom for event markers above max latency / WCL; the pNN
    y-cap covers the maximum p99 observed; the histogram x-range
    spans the union of populated log bins, floored at hist_min_ms_floor."""
    lat_max = 0.0
    pnn_max = 0.0
    hist_min_idx = HIST_N_LOG_BINS  # default: nothing populated
    hist_max_idx = -1
    for df, meta, hist, _ in summaries:
        mask = df["count"].to_numpy() > 0
        if mask.any():
            lat_max = max(lat_max, float(df["max_lat_ns"].to_numpy()[mask].max()) / 1e6)
        lat_max = max(lat_max, meta.wcl_ns / 1e6)
        pNN = _bucket_percentiles(hist, [99.0])
        finite = pNN[~np.isnan(pNN)]
        if finite.size:
            pnn_max = max(pnn_max, float(finite.max()) / 1e6)
        # Histogram x-range from populated log bins.
        log_totals = hist.sum(axis=0)[1 : HIST_N_LOG_BINS + 1]
        nz = np.nonzero(log_totals)[0]
        if nz.size:
            hist_min_idx = min(hist_min_idx, int(nz[0]))
            hist_max_idx = max(hist_max_idx, int(nz[-1]))
    # The latency data (min-max band, mean, WCL line) all lives at or
    # below `lat_max`. The top 25% of the panel is reserved as a lane
    # for the categorical event markers — see `_draw_latency_panel`.
    latency_data_top_ms = lat_max if lat_max > 0 else 0.8
    latency_y_max_ms = lat_max * 1.25 if lat_max > 0 else 1.0
    pnn_y_max_ms = pnn_max * 1.1 if pnn_max > 0 else 1.0
    if hist_max_idx >= 0:
        # Convert bin indices to ms (bin centers).
        centers_ms = HIST_BIN_CENTERS_NS / 1e6
        hist_x_min_ms = max(hist_min_ms_floor, centers_ms[hist_min_idx] * 0.5)
        hist_x_max_ms = centers_ms[hist_max_idx] * 2.0
    else:
        hist_x_min_ms = hist_min_ms_floor
        hist_x_max_ms = HIST_LOG_HIGH_NS / 1e6
    # Always include the WCL line in the histogram x-range so the red
    # dashed marker stays visible on linear scale even when the
    # observed data is well under WCL.
    wcl_ms_max = max((m.wcl_ns for _, m, _, _ in summaries), default=0) / 1e6
    if wcl_ms_max > 0:
        hist_x_max_ms = max(hist_x_max_ms, wcl_ms_max * 1.1)
    return _SharedRanges(
        latency_y_max_ms=latency_y_max_ms,
        latency_data_top_ms=latency_data_top_ms,
        pnn_y_max_ms=pnn_y_max_ms,
        hist_x_min_ms=hist_x_min_ms,
        hist_x_max_ms=hist_x_max_ms,
    )


def _load_timer_histogram_csv(path: Path) -> dict:
    """Load a wan-timer histogram CSV produced by owlm_tool's
    --wan-timer-wake-stats-csv / --wan-timer-duration-stats-csv. Schema:
    bin_low_ns,bin_high_ns,count. The underflow row has an empty
    bin_low_ns and the overflow row has an empty bin_high_ns (parsed
    as NaN). Returns a dict with bin_lows_ns, bin_highs_ns, counts (all
    numpy arrays, in-range only), plus underflow_count and
    overflow_count (scalars), and config_low_ns / config_high_ns
    extracted from the two overflow rows."""
    df = pd.read_csv(path)
    under_mask = df["bin_low_ns"].isna()
    over_mask = df["bin_high_ns"].isna()
    in_range = df[~(under_mask | over_mask)]
    underflow_count = int(df.loc[under_mask, "count"].sum()) if under_mask.any() else 0
    overflow_count = int(df.loc[over_mask, "count"].sum()) if over_mask.any() else 0
    # The underflow row's bin_high_ns is the histogram's configured low;
    # the overflow row's bin_low_ns is the configured high.
    config_low_ns = (
        int(df.loc[under_mask, "bin_high_ns"].iloc[0])
        if under_mask.any()
        else int(in_range["bin_low_ns"].min())
    )
    config_high_ns = (
        int(df.loc[over_mask, "bin_low_ns"].iloc[0])
        if over_mask.any()
        else int(in_range["bin_high_ns"].max())
    )
    return {
        "bin_lows_ns": in_range["bin_low_ns"].to_numpy(dtype=np.int64),
        "bin_highs_ns": in_range["bin_high_ns"].to_numpy(dtype=np.int64),
        "counts": in_range["count"].to_numpy(dtype=np.int64),
        "underflow_count": underflow_count,
        "overflow_count": overflow_count,
        "config_low_ns": config_low_ns,
        "config_high_ns": config_high_ns,
    }


def _load_outliers_csv(path: Path) -> pd.DataFrame:
    """Load the per-outlier CSV produced by `owlm_analyze.py events`.
    Expected columns: rx_gptp_ns, rx_pdt, presentation_time_ns,
    latency_ns, latency_ms, sender_eui64, sequence, interval_us, role,
    direction. Empty / header-only files return an empty DataFrame."""
    return pd.read_csv(path)


def _draw_timer_histogram_panel(ax, hist_data: dict, kind: str, label: str) -> None:
    """Bar plot of a wan-timer histogram (wake error or callback
    duration). X axis in microseconds; counts are shown on a symlog y
    axis so single-count bins remain visible alongside the mode bin.
    Underflow / overflow tails are annotated in the title (no separate
    bars — they sit outside the configured range)."""
    centers_us = ((hist_data["bin_lows_ns"] + hist_data["bin_highs_ns"]) / 2.0) / 1000.0
    widths_us = (hist_data["bin_highs_ns"] - hist_data["bin_lows_ns"]) / 1000.0
    counts = hist_data["counts"]
    ax.bar(
        centers_us,
        counts,
        width=widths_us,
        align="center",
        color="steelblue",
        edgecolor="none",
    )
    ax.set_yscale("symlog", linthresh=1)
    ax.axvline(0, color="0.6", linestyle=":", linewidth=0.8)
    ax.set_xlabel(f"{kind} (µs)")
    ax.set_ylabel("count (symlog)")
    total = (
        int(counts.sum()) + hist_data["underflow_count"] + hist_data["overflow_count"]
    )
    title = f"{label}: wan-timer {kind} histogram  (total samples: {total:,})"
    tails = []
    if hist_data["underflow_count"] > 0:
        tails.append(
            f"underflow (<{hist_data['config_low_ns'] / 1000:.1f} µs): {hist_data['underflow_count']:,}"
        )
    if hist_data["overflow_count"] > 0:
        tails.append(
            f"overflow (≥{hist_data['config_high_ns'] / 1000:.1f} µs): {hist_data['overflow_count']:,}"
        )
    if tails:
        title += "\n" + ", ".join(tails)
    ax.set_title(title)


def _draw_outlier_scatter_panel(ax, df: pd.DataFrame, t0_ns: int, label: str) -> None:
    """One dot per outlier: X = session-time (s), Y = latency (ms),
    coloured by direction (low/high). Sparse-event-friendly — designed
    for the 'a few outliers across hours' case where a histogram alone
    hides the timing structure."""
    if df.empty:
        ax.text(
            0.5,
            0.5,
            "no outliers",
            transform=ax.transAxes,
            ha="center",
            va="center",
            fontsize=10,
            color="0.5",
        )
        ax.set_title(f"{label}: outliers (none)")
        ax.set_xlabel("session time (s)")
        ax.set_ylabel("latency (ms)")
        return
    t_s = (df["rx_gptp_ns"].to_numpy(dtype=np.int64) - t0_ns) / 1e9
    lat_ms = df["latency_ms"].to_numpy()
    direction = df["direction"].astype(str).to_numpy()
    low = direction == "low"
    high = direction == "high"
    if low.any():
        ax.scatter(
            t_s[low],
            lat_ms[low],
            c="tab:blue",
            s=18,
            marker="v",
            label=f"low ({int(low.sum())})",
            zorder=3,
        )
    if high.any():
        ax.scatter(
            t_s[high],
            lat_ms[high],
            c="tab:red",
            s=18,
            marker="^",
            label=f"high ({int(high.sum())})",
            zorder=3,
        )
    ax.axhline(0, color="0.6", linestyle=":", linewidth=0.8)
    ax.set_xlabel("session time (s)")
    ax.set_ylabel("latency (ms)")
    ax.set_title(f"{label}: outliers vs time  ({len(df):,} total)")
    ax.legend(loc="upper right", fontsize=8)


def _draw_outlier_histogram_panel(ax, df: pd.DataFrame, label: str) -> None:
    """Histogram of outlier latency_ms split by direction (low / high
    stacked on the same axis with different colours). For the typical
    case of 3–thousands of outliers this surfaces the modal magnitude
    of each tail clearly."""
    if df.empty:
        ax.text(
            0.5,
            0.5,
            "no outliers",
            transform=ax.transAxes,
            ha="center",
            va="center",
            fontsize=10,
            color="0.5",
        )
        ax.set_title(f"{label}: outliers distribution (none)")
        ax.set_xlabel("latency (ms)")
        ax.set_ylabel("count")
        return
    lat_ms = df["latency_ms"].to_numpy()
    direction = df["direction"].astype(str).to_numpy()
    low = lat_ms[direction == "low"]
    high = lat_ms[direction == "high"]
    n_bins = max(10, min(40, len(df) // 4 if len(df) > 40 else 10))
    series = []
    colors = []
    labels = []
    if low.size:
        series.append(low)
        colors.append("tab:blue")
        labels.append(f"low ({low.size})")
    if high.size:
        series.append(high)
        colors.append("tab:red")
        labels.append(f"high ({high.size})")
    if series:
        ax.hist(series, bins=n_bins, color=colors, label=labels, stacked=False)
    ax.axvline(0, color="0.6", linestyle=":", linewidth=0.8)
    ax.set_xlabel("latency (ms)")
    ax.set_ylabel("count")
    ax.set_title(f"{label}: outliers distribution  ({len(df):,} total)")
    ax.legend(loc="upper right", fontsize=8)


def cmd_plot(args: argparse.Namespace) -> int:
    """Render one or two summary parquets to a PDF/PNG. Layout is
    full-width rows: latency-A, latency-B, pNN-A, pNN-B, hist-A,
    hist-B. With a single device, just the three A rows. Optional
    extra rows for wan-timer wake/duration histograms and per-packet
    outlier scatter+distribution panels, gated on which of the
    --wake-csv-*, --duration-csv-*, --outliers-csv-* flags were set.
    Y-axis on matching panels is shared so A and B are visually
    comparable."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if len(args.inputs) < 1 or len(args.inputs) > 2:
        print("error: plot accepts 1 or 2 summary parquets", file=sys.stderr)
        return 2

    summaries = []
    for path in args.inputs:
        p = Path(path)
        df, meta, hist = load_summary(p)
        label = (
            args.label_a
            if (path == args.inputs[0] and args.label_a)
            else args.label_b
            if (len(args.inputs) > 1 and path == args.inputs[1] and args.label_b)
            else p.stem
        )
        summaries.append((df, meta, hist, label))

    ranges = _compute_shared_ranges(summaries, hist_min_ms_floor=args.hist_min_ms)

    n_devices = len(summaries)
    panels = [_draw_latency_panel, _draw_pnn_panel, _draw_histogram_panel]

    # Optional panels — built as a list of zero-arg draw closures so
    # they slot into the figure regardless of which subset was passed.
    # Each entry corresponds to one row in the output. Order matches
    # the existing convention (panel-kind major, device minor): all
    # wake rows first, then duration rows, then outlier scatter rows,
    # then outlier histogram rows. The same device label propagates
    # into the title via the captured `label`.
    extra_rows: list[callable] = []
    per_device_meta = [s[1] for s in summaries]  # for t0 alignment on outliers
    per_device_label = [s[3] for s in summaries]

    def _label_for(dev_idx: int, override: str | None) -> str:
        if override:
            return override
        if dev_idx < len(per_device_label):
            return per_device_label[dev_idx]
        return ("A", "B")[dev_idx] if dev_idx < 2 else f"dev{dev_idx}"

    # wan-timer wake-error histograms
    for dev_idx, csv_path in enumerate([args.wake_csv_a, args.wake_csv_b]):
        if not csv_path:
            continue
        data = _load_timer_histogram_csv(Path(csv_path))
        lbl = _label_for(dev_idx, args.label_a if dev_idx == 0 else args.label_b)
        extra_rows.append(
            lambda ax, d=data, l=lbl: _draw_timer_histogram_panel(
                ax, d, "wake error", l
            )
        )

    # wan-timer callback-duration histograms
    for dev_idx, csv_path in enumerate([args.duration_csv_a, args.duration_csv_b]):
        if not csv_path:
            continue
        data = _load_timer_histogram_csv(Path(csv_path))
        lbl = _label_for(dev_idx, args.label_a if dev_idx == 0 else args.label_b)
        extra_rows.append(
            lambda ax, d=data, l=lbl: _draw_timer_histogram_panel(
                ax, d, "callback duration", l
            )
        )

    # Outlier scatter + outlier histogram per device (two rows each).
    outlier_dfs: list[tuple[int, pd.DataFrame, str]] = []
    for dev_idx, csv_path in enumerate([args.outliers_csv_a, args.outliers_csv_b]):
        if not csv_path:
            continue
        odf = _load_outliers_csv(Path(csv_path))
        lbl = _label_for(dev_idx, args.label_a if dev_idx == 0 else args.label_b)
        outlier_dfs.append((dev_idx, odf, lbl))

    # Scatter rows first, then histogram rows — keeps device-by-device
    # comparison clean when both A and B have outliers.
    for dev_idx, odf, lbl in outlier_dfs:
        # Prefer the matching summary's t0 for cross-device alignment;
        # fall back to the outliers CSV's own min(rx_gptp_ns).
        if dev_idx < len(per_device_meta):
            t0 = int(per_device_meta[dev_idx].t0_ns)
        elif not odf.empty:
            t0 = int(odf["rx_gptp_ns"].min())
        else:
            t0 = 0
        extra_rows.append(
            lambda ax, df=odf, t=t0, l=lbl: _draw_outlier_scatter_panel(ax, df, t, l)
        )
    for _, odf, lbl in outlier_dfs:
        extra_rows.append(
            lambda ax, df=odf, l=lbl: _draw_outlier_histogram_panel(ax, df, l)
        )

    n_core_rows = len(panels) * n_devices
    n_data_rows = n_core_rows + len(extra_rows)

    # Stats header sits at the top of the figure. It carries no data
    # itself, just per-device counters + required-buffer figures, so it
    # can be much shorter than a data row and is sized independently
    # via --stats-header-inches (set to 0 to suppress).
    stats_inches = max(0.0, float(args.stats_header_inches))
    show_header = stats_inches > 0.0
    data_inches = args.row_height_inches * n_data_rows
    fig_h = data_inches + stats_inches
    fig = plt.figure(figsize=(args.width_inches, fig_h))
    if show_header:
        gs = fig.add_gridspec(
            n_data_rows + 1,
            1,
            height_ratios=[stats_inches] + [args.row_height_inches] * n_data_rows,
        )
        header_ax = fig.add_subplot(gs[0])
        _draw_stats_header_panel(header_ax, summaries, list(args.stats_percentiles))
        data_axes = [fig.add_subplot(gs[i + 1]) for i in range(n_data_rows)]
    else:
        gs = fig.add_gridspec(n_data_rows, 1)
        data_axes = [fig.add_subplot(gs[i]) for i in range(n_data_rows)]
    axes = data_axes

    for panel_idx, draw in enumerate(panels):
        for dev_idx, (df, meta, hist, label) in enumerate(summaries):
            ax = axes[panel_idx * n_devices + dev_idx]
            draw(ax, df, meta, hist, label, ranges)
    for i, draw_extra in enumerate(extra_rows):
        draw_extra(axes[n_core_rows + i])

    if args.title:
        fig.suptitle(args.title, fontsize=12, y=1.001)
    fig.tight_layout()

    output_path = args.output
    fmt = args.format
    if fmt is None:
        suffix = Path(output_path).suffix.lower()
        fmt = "png" if suffix == ".png" else "pdf"
    expected_suffix = f".{fmt}"
    if Path(output_path).suffix.lower() != expected_suffix:
        output_path = str(Path(output_path).with_suffix(expected_suffix))
    if fmt == "pdf":
        fig.savefig(output_path, format="pdf")
    else:
        fig.savefig(output_path, format="png", dpi=args.dpi)
    print(f"wrote {output_path}")
    return 0


def cmd_join(args: argparse.Namespace) -> int:
    """Joint A↔B summary from two pre-summarized parquet files."""
    df_a, meta_a, hist_a = load_summary(Path(args.summary_a))
    df_b, meta_b, hist_b = load_summary(Path(args.summary_b))

    def overall(df: pd.DataFrame) -> dict:
        cnt = df["count"].to_numpy()
        nonempty = cnt > 0
        if not nonempty.any():
            return {
                "count": 0,
                "min_ms": float("nan"),
                "mean_ms": float("nan"),
                "max_ms": float("nan"),
                "late": 0,
                "recovered": 0,
                "true_loss": 0,
            }
        return {
            "count": int(cnt.sum()),
            "min_ms": float(df["min_lat_ns"].to_numpy()[nonempty].min()) / 1e6,
            "mean_ms": float(df["sum_lat_ns"].sum()) / max(int(cnt.sum()), 1) / 1e6,
            "max_ms": float(df["max_lat_ns"].to_numpy()[nonempty].max()) / 1e6,
            "late": int(df["late_count"].sum()),
            "recovered": int(df["recovered_count"].sum()),
            "true_loss": int(df["true_loss_count"].sum()),
        }

    a_to_b = overall(df_b)  # B received from A
    b_to_a = overall(df_a)  # A received from B

    print(
        f"Joint A↔B summary (bucket A={meta_a.bucket_ns / 1e6:.1f}ms, "
        f"B={meta_b.bucket_ns / 1e6:.1f}ms)"
    )
    print(f"{'metric':<14} {'A→B':>14} {'B→A':>14}")
    for k, fmt in [
        ("count", "d"),
        ("min_ms", ".3f"),
        ("mean_ms", ".3f"),
        ("max_ms", ".3f"),
        ("late", "d"),
        ("recovered", "d"),
        ("true_loss", "d"),
    ]:
        va = a_to_b[k]
        vb = b_to_a[k]
        if fmt == "d":
            print(f"{k:<14} {int(va):>14} {int(vb):>14}")
        else:
            print(f"{k:<14} {va:>14{fmt}} {vb:>14{fmt}}")

    if not np.isnan(a_to_b["min_ms"]) and not np.isnan(b_to_a["min_ms"]):
        floor_delta = a_to_b["min_ms"] - b_to_a["min_ms"]
        print(f"\nmin_floor_delta  = {floor_delta:+.3f} ms")

    # Percentile latency + required receive buffer.
    #
    # For a receiver that does not normalise to a non-zero minimum (the
    # statusbar/AVB model), the jitter buffer it needs at reliability
    # target P is the span between the run's best-observed latency
    # (min) and the worst-case at percentile P:
    #
    #     buffer_required(P) = pX_latency - min_latency
    #
    # pX comes from the per-bucket log-spaced histograms summed across
    # the run; values are interpolated log-linearly within the chosen
    # bin so the answer is meaningful even when the target sits near
    # the top edge of the highest populated bin.
    pcts = sorted(set(float(p) for p in args.percentiles)) if args.percentiles else []
    have_floor = not (np.isnan(a_to_b["min_ms"]) or np.isnan(b_to_a["min_ms"]))
    if pcts and have_floor:
        overall_hist = np.vstack(
            [hist_b.sum(axis=0), hist_a.sum(axis=0)]
        )  # row 0 = A→B (received by B), row 1 = B→A
        pct_ns = _bucket_percentiles_interp(overall_hist, pcts)
        pct_ms_ab = pct_ns[0] / 1e6
        pct_ms_ba = pct_ns[1] / 1e6

        print()
        print(
            "Required receive buffer at percentile "
            "(buffer = percentile latency − min latency)"
        )
        print(
            f"{'percentile':<10} "
            f"{'A→B lat_ms':>12} {'A→B buf_ms':>12} "
            f"{'B→A lat_ms':>12} {'B→A buf_ms':>12}"
        )
        for p, ab_lat, ba_lat in zip(pcts, pct_ms_ab, pct_ms_ba):
            label = f"p{p:g}"
            ab_buf = ab_lat - a_to_b["min_ms"]
            ba_buf = ba_lat - b_to_a["min_ms"]
            print(
                f"{label:<10} "
                f"{ab_lat:>12.3f} {ab_buf:>12.3f} "
                f"{ba_lat:>12.3f} {ba_buf:>12.3f}"
            )
    return 0


def extract_events(df: pd.DataFrame, threshold_ns: int, gap_ns: int) -> pd.DataFrame:
    """Group consecutive outliers into events. A new event starts when
    the previous outlier's rx_gptp_ns is more than `gap_ns` before the
    current one's. Operates on a fully-loaded raw CSV — retained for
    library callers / inline tests; the `events` CLI command no longer
    uses this path."""
    primary = remote_primary(df).sort_values("rx_gptp_ns").reset_index(drop=True)
    over = primary[primary["latency_ns"] > threshold_ns].reset_index(drop=True)
    if over.empty:
        return pd.DataFrame(
            columns=["start_rx_gptp_ns", "end_rx_gptp_ns", "peak_latency_ns", "count"]
        )

    rx = over["rx_gptp_ns"].to_numpy()
    new_event = np.concatenate(([True], (rx[1:] - rx[:-1]) > gap_ns))
    over = over.assign(_event_id=np.cumsum(new_event))

    grouped = (
        over.groupby("_event_id")
        .agg(
            start_rx_gptp_ns=("rx_gptp_ns", "min"),
            end_rx_gptp_ns=("rx_gptp_ns", "max"),
            peak_latency_ns=("latency_ns", "max"),
            count=("latency_ns", "size"),
        )
        .reset_index(drop=True)
    )
    return grouped


def _eui64_u64_to_colon_hex(u64_val: int) -> str:
    """Format a sender_eui64 column value (u64 view of 8 network-byte-
    order bytes on a little-endian host) as 'xx:xx:xx:xx:xx:xx:xx:xx'.
    Network byte 0 (MSB) is u64 bits 0–7 on LE; byte 7 is bits 56–63."""
    v = int(u64_val)
    return ":".join(f"{(v >> (8 * b)) & 0xFF:02x}" for b in range(8))


def _iter_input_chunks(path: Path, chunksize: int):
    """Unified streaming reader over .csv / .csv.gz / .colbin. Yields
    pandas DataFrames in `chunksize`-row slices with `sender_eui64`
    normalised to the colon-hex string form (matching CSV input), so
    downstream filtering / output is format-agnostic."""
    if _is_colbin(path):
        for chunk in _iter_colbin_chunks(path, chunksize):
            if "sender_eui64" in chunk.columns:
                u64s = chunk["sender_eui64"].to_numpy()
                chunk = chunk.copy()
                chunk["sender_eui64"] = [_eui64_u64_to_colon_hex(v) for v in u64s]
            yield chunk
        return
    reader = pd.read_csv(
        path,
        dtype=CSV_DTYPES,
        chunksize=chunksize,
        on_bad_lines="skip",
    )
    for chunk in _iter_chunks_tolerant(reader, label=path.name):
        yield chunk


# Stable column order for the events CSV output.
_OUTLIER_COLUMNS = [
    "rx_gptp_ns",
    "rx_pdt",
    "presentation_time_ns",
    "latency_ns",
    "latency_ms",
    "sender_eui64",
    "sequence",
    "interval_us",
    "role",
    "direction",
]


def collect_outliers(
    path: Path,
    lower_threshold_ns: int | None,
    upper_threshold_ns: int | None,
    chunksize: int,
    window_t0_ns: int | None = None,
    window_t1_ns: int | None = None,
) -> tuple[pd.DataFrame, int, pd.DataFrame | None]:
    """Stream `path` and collect per-packet outlier rows where
    `latency_ns` is below `lower_threshold_ns` (if provided) or above
    `upper_threshold_ns` (if provided). Filtered to roles
    remote_primary + remote_legacy (the cross-clock measurements the
    `min_ms` / `max_ms` columns in `join` come from).

    Optional `[window_t0_ns, window_t1_ns]` excludes outliers whose
    `rx_gptp_ns` falls outside this window (matching `summarize`'s trim
    semantics so the outliers CSV and the plot/summary agree on which
    packets count). Outliers outside the window are still threshold-
    matched and returned separately as `trimmed_out_df` so the caller
    can report on what was hidden — pass `None` for either bound to
    disable trimming on that side.

    Returns ``(kept_df, n_trimmed, trimmed_out_df)``:
        - ``kept_df`` — outliers within the trim window (the canonical CSV).
        - ``n_trimmed`` — count of outliers excluded by the trim window.
        - ``trimmed_out_df`` — those excluded rows (or None if no trim was
          requested), for the caller's stderr note.
    """
    kept_pieces: list[pd.DataFrame] = []
    trimmed_pieces: list[pd.DataFrame] = []
    role_filter = ("remote_primary", "remote_legacy")
    trimming = window_t0_ns is not None or window_t1_ns is not None
    for chunk in _iter_input_chunks(path, chunksize):
        primary = chunk[chunk["role"].isin(role_filter)]
        if primary.empty:
            continue
        lat = primary["latency_ns"].to_numpy()
        low_mask = (
            (lat < lower_threshold_ns)
            if lower_threshold_ns is not None
            else np.zeros(lat.shape, dtype=bool)
        )
        high_mask = (
            (lat > upper_threshold_ns)
            if upper_threshold_ns is not None
            else np.zeros(lat.shape, dtype=bool)
        )
        mask = low_mask | high_mask
        if not mask.any():
            continue
        sub = primary[mask].copy()
        sub["direction"] = np.where(low_mask[mask], "low", "high")
        if trimming:
            rx = sub["rx_gptp_ns"].to_numpy()
            in_window = np.ones(rx.shape, dtype=bool)
            if window_t0_ns is not None:
                in_window &= rx >= window_t0_ns
            if window_t1_ns is not None:
                in_window &= rx < window_t1_ns
            if in_window.any():
                kept_pieces.append(sub[in_window])
            if (~in_window).any():
                trimmed_pieces.append(sub[~in_window])
        else:
            kept_pieces.append(sub)

    def _finalize(pieces: list[pd.DataFrame]) -> pd.DataFrame:
        if not pieces:
            return pd.DataFrame(columns=_OUTLIER_COLUMNS)
        out = pd.concat(pieces, ignore_index=True)
        out = out.sort_values("rx_gptp_ns").reset_index(drop=True)
        out["latency_ms"] = out["latency_ns"].astype("float64") / 1e6
        out["rx_pdt"] = [
            _tai_ns_to_pdt_str(int(v)) for v in out["rx_gptp_ns"].to_numpy()
        ]
        cols = [c for c in _OUTLIER_COLUMNS if c in out.columns]
        return out[cols]

    kept_df = _finalize(kept_pieces)
    trimmed_df = _finalize(trimmed_pieces) if trimming else None
    n_trimmed = len(trimmed_df) if trimmed_df is not None else 0
    return kept_df, n_trimmed, trimmed_df


def cmd_events(args: argparse.Namespace) -> int:
    # Resolve thresholds. --threshold-ms is the legacy single-sided
    # alias; --upper-threshold-ms overrides it when both are given. If
    # neither upper bound is supplied, default to 5 ms (preserves the
    # historic default behavior). --negative-threshold-ms is opt-in;
    # leave it unset to skip negative-side collection.
    upper_ms = (
        args.upper_threshold_ms
        if args.upper_threshold_ms is not None
        else (args.threshold_ms if args.threshold_ms is not None else 5.0)
    )
    lower_ms = args.negative_threshold_ms  # None unless explicitly set
    upper_ns: int | None = int(upper_ms * 1e6) if upper_ms is not None else None
    lower_ns: int | None = int(lower_ms * 1e6) if lower_ms is not None else None

    # Trim window — match `summarize`'s default (5 s off each end) so the
    # outliers CSV and the plot/join summary agree by default on what
    # packets count. --trim-seconds=0 sees the full capture (useful for
    # diagnosing warm-up / shutdown transients like NIC ring flushes).
    input_path = Path(args.input)
    window_t0_ns: int | None = None
    window_t1_ns: int | None = None
    trim_ns = int(max(0.0, args.trim_seconds) * 1_000_000_000)
    if trim_ns > 0:
        scan = (
            _scan_metadata_colbin(input_path)
            if _is_colbin(input_path)
            else _scan_metadata(input_path, args.chunksize)
        )
        if scan.t1_ns - scan.t0_ns > 2 * trim_ns:
            window_t0_ns = scan.t0_ns + trim_ns
            window_t1_ns = scan.t1_ns - trim_ns
        else:
            print(
                f"[events] warning: trim_seconds={args.trim_seconds:g} >= half "
                f"the captured span ({(scan.t1_ns - scan.t0_ns) / 1e9:.3f}s); "
                f"skipping trim.",
                file=sys.stderr,
            )

    outliers, n_trimmed, trimmed_df = collect_outliers(
        input_path,
        lower_threshold_ns=lower_ns,
        upper_threshold_ns=upper_ns,
        chunksize=args.chunksize,
        window_t0_ns=window_t0_ns,
        window_t1_ns=window_t1_ns,
    )
    outliers.to_csv(args.output, index=False)

    n = len(outliers)
    low_n = int((outliers["direction"] == "low").sum()) if n else 0
    high_n = int((outliers["direction"] == "high").sum()) if n else 0
    lb_str = f"{lower_ms} ms" if lower_ms is not None else "disabled"
    ub_str = f"{upper_ms} ms" if upper_ms is not None else "disabled"
    print(
        f"wrote {args.output} ({n} outliers; lower={lb_str} → {low_n}, "
        f"upper={ub_str} → {high_n})"
    )
    # Note trimmed transients so the user knows the warm-up / shutdown
    # had outliers even when the in-window count is zero. Mirrors what
    # the user previously saw as a "mystery" between events and summarize.
    if n_trimmed > 0 and trimmed_df is not None and window_t0_ns is not None:
        earliest = trimmed_df.iloc[0]
        capture_start = window_t0_ns - trim_ns
        offset_s = (int(earliest["rx_gptp_ns"]) - capture_start) / 1e9
        print(
            f"[events] note: trim_seconds={args.trim_seconds:g} excluded "
            f"{n_trimmed} outlier row(s) from the warm-up / shutdown window",
            file=sys.stderr,
        )
        print(
            f"               earliest excluded: {offset_s:+.3f} s from capture "
            f"start, latency {float(earliest['latency_ms']):.3f} ms, "
            f"seq {int(earliest['sequence'])}",
            file=sys.stderr,
        )
        print(
            "               run with --trim-seconds=0 to include them in the CSV.",
            file=sys.stderr,
        )

    if n > 0 and args.preview > 0:
        preview = outliers.head(args.preview)
        # Stdout view: PDT + raw rx & packet timestamps + latency + sender + seq.
        show = preview[
            [
                "rx_pdt",
                "rx_gptp_ns",
                "presentation_time_ns",
                "latency_ms",
                "direction",
                "sender_eui64",
                "sequence",
                "role",
            ]
        ]
        with pd.option_context("display.max_colwidth", None, "display.width", 240):
            print(show.to_string(index=False))
        if n > args.preview:
            print(f"... ({n - args.preview} more rows in {args.output})")
    return 0


# ----------------------------------------------------------------------
# gaps — bursts of consecutive true-loss (how long a drop burst lasted)
# ----------------------------------------------------------------------


def _nominal_interval_ns(rx_at_seq: np.ndarray, seen: np.ndarray) -> int | None:
    """Median per-sequence rx spacing among delivered/seen packets, used
    to estimate the magnitude of a drop burst that touches the capture
    start or end (where there is no bounding delivered packet on one
    side). Returns None when fewer than two packets were seen."""
    idx = np.where(seen)[0]
    if idx.size < 2:
        return None
    rxs = rx_at_seq[idx].astype(np.float64)
    seq_gaps = np.diff(idx).astype(np.float64)
    per = np.diff(rxs) / seq_gaps
    per = per[per > 0]
    if per.size == 0:
        return None
    return int(np.median(per))


def _derive_gaps(
    merged: _PartialResult,
    pair_seq_ranges: dict,
    wcl_ns: int,
    min_dropped: int = 1,
) -> list[dict]:
    """Walk each logical sender's [min_seq, max_seq] range and group
    consecutive true-loss sequences into 'gap' bursts.

    A sequence is *delivered* iff at least one copy (primary/legacy or
    redundant) arrived within WCL — identical to the summarize / join
    loss model. A *gap* is a maximal run of non-delivered sequences;
    genuine never-seen holes and all-copies-late sequences both count.

    Each gap's burst duration is the outage between the last delivered
    packet before it and the first delivered packet after it
    (``outage_ns``). Gaps that touch the capture start or end have no
    bounding packet on that side: ``bounded`` is False and ``outage_ns``
    is None; ``est_dropped_ns`` (count x nominal interval) is still
    reported as a fallback magnitude. Materialization of the per-seq
    arrays mirrors `_derive_recovered_true_loss`."""
    int64_max = np.iinfo(np.int64).max
    records: list[dict] = []
    n_discontinuities = 0

    for pid, (min_seq, max_seq) in pair_seq_ranges.items():
        span = max_seq - min_seq + 1
        if span <= 0:
            continue
        primary_minlat = np.full(span, int64_max, dtype=np.int64)
        redundant_minlat = np.full(span, int64_max, dtype=np.int64)
        rx_at_seq = np.full(span, int64_max, dtype=np.int64)

        if pid in merged.pair_primary_seqs:
            idx = merged.pair_primary_seqs[pid] - min_seq
            m = (idx >= 0) & (idx < span)
            np.minimum.at(primary_minlat, idx[m], merged.pair_primary_minlat[pid][m])
        if pid in merged.pair_redundant_seqs:
            idx = merged.pair_redundant_seqs[pid] - min_seq
            m = (idx >= 0) & (idx < span)
            np.minimum.at(
                redundant_minlat, idx[m], merged.pair_redundant_minlat[pid][m]
            )
        if pid in merged.pair_first_seqs:
            idx = merged.pair_first_seqs[pid] - min_seq
            m = (idx >= 0) & (idx < span)
            np.minimum.at(rx_at_seq, idx[m], merged.pair_first_rxs[pid][m])

        delivered = (primary_minlat <= wcl_ns) | (redundant_minlat <= wcl_ns)
        if delivered.all():
            continue
        dropped = ~delivered
        seen = rx_at_seq != int64_max
        nominal = _nominal_interval_ns(rx_at_seq, seen)

        # Maximal runs of dropped sequences via a padded rising/falling
        # edge diff: +1 marks a run start, -1 the position past a run end.
        padded = np.concatenate(([0], dropped.astype(np.int8), [0]))
        edges = np.diff(padded)
        starts = np.where(edges == 1)[0]
        ends = np.where(edges == -1)[0] - 1

        for a, b in zip(starts, ends):
            count = int(b - a + 1)
            if count < min_dropped:
                continue
            # The sequence just before a maximal dropped run is delivered
            # (so it was seen); likewise the one just after — unless the
            # run touches the capture's first / last seen sequence.
            last_good_off = int(a) - 1 if a > 0 else None
            first_good_off = int(b) + 1 if b < span - 1 else None
            last_good_rx = (
                int(rx_at_seq[last_good_off]) if last_good_off is not None else None
            )
            first_good_rx = (
                int(rx_at_seq[first_good_off]) if first_good_off is not None else None
            )
            bounded = last_good_rx is not None and first_good_rx is not None
            # Sequence discontinuity (talker restart / reset): a bounded run
            # whose good packets are out of time order -- the higher-seq one
            # arrived no later than the lower-seq one. Not a real outage
            # (would otherwise report a phantom burst, often with negative
            # duration). Skip it; surface the count instead of silently
            # dropping it.
            if bounded and first_good_rx <= last_good_rx:
                n_discontinuities += 1
                continue
            outage_ns = (first_good_rx - last_good_rx) if bounded else None
            est_dropped_ns = count * nominal if nominal is not None else None
            rep_rx = last_good_rx if last_good_rx is not None else first_good_rx
            records.append(
                {
                    "pid": pid,
                    "start_seq": int(min_seq + a),
                    "end_seq": int(min_seq + b),
                    "dropped_count": count,
                    "last_good_seq": (
                        int(min_seq + last_good_off)
                        if last_good_off is not None
                        else None
                    ),
                    "last_good_rx_gptp_ns": last_good_rx,
                    "first_good_seq": (
                        int(min_seq + first_good_off)
                        if first_good_off is not None
                        else None
                    ),
                    "first_good_rx_gptp_ns": first_good_rx,
                    "outage_ns": outage_ns,
                    "est_dropped_ns": est_dropped_ns,
                    "bounded": bounded,
                    "_rep_rx": rep_rx,
                }
            )
    return records, n_discontinuities


# `start_t_ns` + `duration_ns` are the canonical pair consumed by the
# cross-run aggregator (session_plots.py): the outage begins at
# start_t_ns on the receiver's rx/gptp wall clock and lasts duration_ns.
# Both are always integers (never NaN) so downstream int64 reads are safe;
# the remaining columns are human-facing detail.
_GAP_COLUMNS = [
    "sender_eui64",
    "start_seq",
    "end_seq",
    "dropped_count",
    "start_t_ns",
    "duration_ns",
    "duration_ms",
    "bounded",
    "last_good_seq",
    "last_good_rx_gptp_ns",
    "first_good_seq",
    "first_good_rx_gptp_ns",
    "est_dropped_ms",
    "start_pdt",
]


def _format_pair_id(pid) -> str:
    """Render a pair id (uint64 from .colbin, or colon-hex string from
    CSV) as a stable 'xx:..:xx' string for the output CSV."""
    if isinstance(pid, (int, np.integer)):
        return _eui64_u64_to_colon_hex(int(pid))
    return str(pid)


def _gaps_to_df(records: list[dict]) -> pd.DataFrame:
    """Sort gap records by start time and project to the output schema."""
    if not records:
        return pd.DataFrame(columns=_GAP_COLUMNS)
    records = sorted(records, key=lambda r: (r["_rep_rx"] is None, r["_rep_rx"] or 0))
    rows = []
    for r in records:
        outage_ns = r["outage_ns"]
        est_ns = r["est_dropped_ns"]
        last_good = r["last_good_rx_gptp_ns"]
        first_good = r["first_good_rx_gptp_ns"]
        rep = r["_rep_rx"]
        # Canonical (always-int) outage span. A bounded gap uses the real
        # last-good → first-good window; an edge gap (no bounding packet on
        # one side) falls back to count × nominal interval, anchored so the
        # outage still ends at the next delivered packet when one exists.
        if outage_ns is not None:
            duration_ns = int(outage_ns)
        elif est_ns is not None:
            duration_ns = int(est_ns)
        else:
            duration_ns = 0
        if last_good is not None:
            start_t_ns = int(last_good)
        elif first_good is not None:
            start_t_ns = int(first_good) - duration_ns
        else:
            start_t_ns = int(rep) if rep is not None else 0
        rows.append(
            {
                "sender_eui64": _format_pair_id(r["pid"]),
                "start_seq": r["start_seq"],
                "end_seq": r["end_seq"],
                "dropped_count": r["dropped_count"],
                "start_t_ns": start_t_ns,
                "duration_ns": duration_ns,
                "duration_ms": duration_ns / 1e6,
                "bounded": r["bounded"],
                "last_good_seq": r["last_good_seq"],
                "last_good_rx_gptp_ns": last_good,
                "first_good_seq": r["first_good_seq"],
                "first_good_rx_gptp_ns": first_good,
                "est_dropped_ms": (est_ns / 1e6) if est_ns is not None else None,
                "start_pdt": _tai_ns_to_pdt_str(start_t_ns),
            }
        )
    return pd.DataFrame(rows, columns=_GAP_COLUMNS)


def _aggregate_to_merged(
    path: Path,
    t0_ns: int,
    bucket_ns: int,
    n_buckets: int,
    wcl_ns: int,
    chunksize: int,
    n_workers: int,
) -> _PartialResult:
    """Run pass-2 aggregation and return the merged per-pair partial,
    dispatching over .colbin (mmap stream), .csv.gz (single stream), or
    plain .csv (byte-range parallel) exactly as `summarize_csv` does. The
    per-bucket arrays are produced but unused by gap detection — only the
    sparse per-pair seq / latency / rx state matters here."""
    if _is_colbin(path):
        return _process_streaming_colbin(
            path, t0_ns, bucket_ns, n_buckets, wcl_ns, chunksize
        )
    if _is_gzipped(path):
        return _process_streaming(path, t0_ns, bucket_ns, n_buckets, wcl_ns, chunksize)
    ranges = _file_byte_ranges(path, n_workers)
    worker_args = [
        (str(path), start, end, t0_ns, bucket_ns, n_buckets, wcl_ns, chunksize)
        for (start, end) in ranges
    ]
    if n_workers > 1 and len(ranges) > 1:
        with mp.get_context("spawn").Pool(n_workers) as pool:
            partials = pool.map(_process_byte_range, worker_args)
    else:
        partials = [_process_byte_range(worker_args[0])]
    return _merge_partials(partials)


def compute_gaps(
    path: Path,
    wcl_ms: float = 25.0,
    chunksize: int = DEFAULT_CHUNKSIZE,
    min_dropped: int = 1,
    n_workers: int | None = None,
) -> pd.DataFrame:
    """Detect drop bursts (runs of consecutive true-loss sequences) per
    logical sender and return them as a DataFrame (see `_GAP_COLUMNS`).
    Reuses the same streaming scan / aggregation path as `summarize`."""
    if n_workers is None:
        n_workers = max(1, os.cpu_count() or 1)
    scan = (
        _scan_metadata_colbin(path)
        if _is_colbin(path)
        else _scan_metadata(path, chunksize)
    )
    span_ns = max(1, scan.t1_ns - scan.t0_ns)
    bucket_ns = max(1, span_ns // DEFAULT_PLOT_WIDTH)
    n_buckets = int(span_ns // bucket_ns) + 1
    wcl_ns = int(wcl_ms * 1_000_000)
    merged = _aggregate_to_merged(
        path, scan.t0_ns, bucket_ns, n_buckets, wcl_ns, chunksize, n_workers
    )
    # The per-pair seq range spans the whole capture (pair_first_seqs is
    # not trimmed), so its min/max bound the gap search per sender.
    full_pair_seq_ranges: dict = {}
    for pid, seqs in merged.pair_first_seqs.items():
        if seqs.size > 0:
            full_pair_seq_ranges[pid] = (int(seqs.min()), int(seqs.max()))
    records, n_discontinuities = _derive_gaps(
        merged, full_pair_seq_ranges, wcl_ns, min_dropped
    )
    df = _gaps_to_df(records)
    df.attrs["n_discontinuities"] = n_discontinuities
    return df


def cmd_gaps(args: argparse.Namespace) -> int:
    input_path = Path(args.input)
    gaps = compute_gaps(
        input_path,
        wcl_ms=args.worst_case_latency_ms,
        chunksize=args.chunksize,
        min_dropped=args.min_dropped,
    )
    gaps.to_csv(args.output, index=False)

    n_disc = int(gaps.attrs.get("n_discontinuities", 0))
    disc_str = (
        f" ({n_disc} sequence discontinuit{'y' if n_disc == 1 else 'ies'} "
        f"skipped — talker restart/reset, not loss)"
        if n_disc
        else ""
    )

    n = len(gaps)
    if n == 0:
        print(
            f"wrote {args.output} (0 gap bursts; no true-loss within "
            f"WCL {args.worst_case_latency_ms:g} ms){disc_str}"
        )
        return 0
    total_dropped = int(gaps["dropped_count"].sum())
    n_senders = gaps["sender_eui64"].nunique()
    bounded = gaps[gaps["bounded"]]
    if not bounded.empty:
        worst = bounded.loc[bounded["duration_ms"].idxmax()]
        worst_str = (
            f"; longest outage {float(worst['duration_ms']):.3f} ms "
            f"(seq {int(worst['start_seq'])}–{int(worst['end_seq'])}, "
            f"{worst['sender_eui64']})"
        )
    else:
        worst_str = ""
    print(
        f"wrote {args.output} ({n} gap burst(s) across {n_senders} sender(s); "
        f"{total_dropped} packet(s) dropped{worst_str}){disc_str}"
    )

    if args.preview > 0:
        show_cols = [
            "start_pdt",
            "sender_eui64",
            "start_seq",
            "end_seq",
            "dropped_count",
            "duration_ms",
            "est_dropped_ms",
            "bounded",
        ]
        preview = gaps.head(args.preview)[show_cols]
        with pd.option_context("display.max_colwidth", None, "display.width", 240):
            print(preview.to_string(index=False))
        if n > args.preview:
            print(f"... ({n - args.preview} more rows in {args.output})")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--test", action="store_true", help="run inline unit tests and exit")
    sub = p.add_subparsers(dest="cmd")

    s = sub.add_parser(
        "summarize",
        help="stream CSV, write small per-bucket summary parquet (use before plot/join)",
    )
    s.add_argument("input", help="owlm CSV input (.csv or .csv.gz)")
    s.add_argument(
        "-o",
        "--output",
        default="summary.parquet",
        help="output parquet (a .json sidecar is also written)",
    )
    s.add_argument(
        "--plot-width",
        type=int,
        default=DEFAULT_PLOT_WIDTH,
        help="target number of pixel-buckets (default 2000)",
    )
    s.add_argument(
        "--chunksize",
        type=int,
        default=DEFAULT_CHUNKSIZE,
        help="CSV streaming chunk size in rows (default 200000)",
    )
    s.add_argument(
        "--worst-case-latency-ms",
        type=float,
        default=60.0,
        help="WCL deadline; latency_ns > this counts as 'late'.",
    )
    s.add_argument(
        "--workers",
        type=int,
        default=None,
        help="Number of multiprocessing workers (default: all CPUs)",
    )
    s.add_argument(
        "--trim-seconds",
        type=float,
        default=5.0,
        help="Discard this many seconds of data from each end of "
        "the capture window before summarizing (default 5.0). "
        "Intended for nominal steady-state long-duration tests "
        "where the first and last few seconds contain warm-up "
        "or shutdown transients. Pass 0 to disable.",
    )
    s.set_defaults(func=cmd_summarize)

    pl = sub.add_parser(
        "plot",
        help="render one or two summary parquets to PDF/PNG. With two "
        "inputs, panels are stacked full-width: latency-A, latency-B, "
        "pNN-A, pNN-B, hist-A, hist-B. Optional wan-timer wake / "
        "duration histogram CSVs and per-device outliers CSVs add "
        "extra rows.",
    )
    pl.add_argument(
        "inputs", nargs="+", help="one or two summary parquets (from `summarize`)"
    )
    pl.add_argument("-o", "--output", default="owlm_plot.pdf")
    pl.add_argument("--width-inches", type=float, default=11.0)
    pl.add_argument(
        "--row-height-inches",
        type=float,
        default=3.0,
        help="height per panel row (default 3 in; 6 rows ≈ 18 in tall)",
    )
    pl.add_argument("--dpi", type=int, default=200)
    pl.add_argument("--title", default=None)
    pl.add_argument("--label-a", default=None, help="title-label for the first input")
    pl.add_argument("--label-b", default=None, help="title-label for the second input")
    pl.add_argument(
        "--stats-header-inches",
        type=float,
        default=2.2,
        help="Height in inches for the per-device stats header at the "
        "top of the figure (packets / late / recovered / true_loss / "
        "required-buffer percentiles). Set to 0 to suppress.",
    )
    pl.add_argument(
        "--stats-percentiles",
        nargs="*",
        type=float,
        default=[99.0, 99.9, 99.99, 99.999],
        metavar="P",
        help="Percentiles to use for the required-buffer rows in the "
        "stats header (default: 99 99.9 99.99 99.999). Pass with no "
        "values to omit the percentile rows.",
    )
    pl.add_argument(
        "--hist-min-ms",
        type=float,
        default=1.0,
        help="X-axis floor for the rx-latency-distribution panel "
        "(default 1.0 ms — small enough to show ~1 ms typical "
        "floors but avoids the empty 100 ns…100 µs region).",
    )
    pl.add_argument(
        "--format",
        choices=["pdf", "png"],
        default=None,
        help="Output format. 'pdf' produces vector graphics "
        "(recommended for publication-quality plots); 'png' "
        "produces raster (smaller for previews). If "
        "unspecified, the format is inferred from the output "
        "filename extension (defaults to PDF).",
    )
    pl.add_argument(
        "--wake-csv-a",
        default=None,
        help="Optional wan-timer wake-error histogram CSV for device A "
        "(produced by owlm_tool --wan-timer-wake-stats-csv).",
    )
    pl.add_argument(
        "--wake-csv-b",
        default=None,
        help="Optional wan-timer wake-error histogram CSV for device B.",
    )
    pl.add_argument(
        "--duration-csv-a",
        default=None,
        help="Optional wan-timer callback-duration histogram CSV for "
        "device A (produced by owlm_tool --wan-timer-duration-stats-csv).",
    )
    pl.add_argument(
        "--duration-csv-b",
        default=None,
        help="Optional wan-timer callback-duration histogram CSV for device B.",
    )
    pl.add_argument(
        "--outliers-csv-a",
        default=None,
        help="Optional per-packet outliers CSV for device A (from "
        "`owlm_analyze.py events`). Renders a scatter + a histogram row.",
    )
    pl.add_argument(
        "--outliers-csv-b",
        default=None,
        help="Optional per-packet outliers CSV for device B.",
    )
    pl.set_defaults(func=cmd_plot)

    j = sub.add_parser("join", help="joint A↔B summary from two summary parquets")
    j.add_argument("summary_a", help="summary parquet for the A endpoint's CSV")
    j.add_argument("summary_b", help="summary parquet for the B endpoint's CSV")
    j.add_argument(
        "--percentiles",
        nargs="*",
        type=float,
        default=[99.0, 99.9, 99.99, 99.999],
        metavar="P",
        help="Report required-receive-buffer at these latency percentiles "
        "(default: 99 99.9 99.99 99.999). Pass `--percentiles` with no "
        "values to suppress the section.",
    )
    j.set_defaults(func=cmd_join)

    ev = sub.add_parser(
        "events",
        help="stream input; collect per-packet outliers below / above thresholds",
    )
    ev.add_argument("input", help="owlm input (.csv, .csv.gz, or .colbin)")
    ev.add_argument(
        "--upper-threshold-ms",
        type=float,
        default=None,
        help="Emit rows where latency_ms exceeds this. If neither --upper-threshold-ms "
        "nor the legacy --threshold-ms is set, defaults to 5.0.",
    )
    ev.add_argument(
        "--negative-threshold-ms",
        type=float,
        default=None,
        help="Emit rows where latency_ms is below this (typically a negative value like "
        "-1.0 to surface clock-step artefacts). Unset = disabled.",
    )
    ev.add_argument(
        "--threshold-ms",
        type=float,
        default=None,
        help="Deprecated alias for --upper-threshold-ms (kept for backward compatibility).",
    )
    ev.add_argument(
        "--chunksize",
        type=int,
        default=DEFAULT_CHUNKSIZE,
        help=f"Streaming chunk size in rows (default {DEFAULT_CHUNKSIZE}).",
    )
    ev.add_argument(
        "--trim-seconds",
        type=float,
        default=5.0,
        help="Discard this many seconds from each end of the captured "
        "window before applying the thresholds. Matches `summarize`'s "
        "default trim so the outliers CSV and the plot/join summary "
        "agree on what packets count. Set to 0 to include startup / "
        "shutdown transients (e.g. NIC ring flushes).",
    )
    ev.add_argument(
        "--preview",
        type=int,
        default=20,
        help="How many rows to print to stdout after writing the CSV (default 20; 0 = none).",
    )
    ev.add_argument("-o", "--output", default="events.csv")
    ev.set_defaults(func=cmd_events)

    g = sub.add_parser(
        "gaps",
        help="detect drop bursts (runs of consecutive true-loss) and how long each lasted",
    )
    g.add_argument("input", help="owlm input (.csv, .csv.gz, or .colbin)")
    g.add_argument(
        "--worst-case-latency-ms",
        type=float,
        default=25.0,
        dest="worst_case_latency_ms",
        help="A sequence counts as delivered only if a primary or redundant "
        "copy arrived within this latency (ms). Sequences with no in-time copy "
        "are 'dropped'; consecutive drops form one gap burst (default 25.0).",
    )
    g.add_argument(
        "--min-dropped",
        type=int,
        default=1,
        dest="min_dropped",
        help="Only report bursts of at least this many consecutive dropped "
        "sequences (default 1).",
    )
    g.add_argument(
        "--chunksize",
        type=int,
        default=DEFAULT_CHUNKSIZE,
        help=f"Streaming chunk size in rows (default {DEFAULT_CHUNKSIZE}).",
    )
    g.add_argument(
        "--preview",
        type=int,
        default=20,
        help="How many rows to print to stdout after writing the CSV (default 20; 0 = none).",
    )
    g.add_argument("-o", "--output", default="gaps.csv")
    g.set_defaults(func=cmd_gaps)

    return p


# ----------------------------------------------------------------------
# Inline tests
# ----------------------------------------------------------------------


def run_tests() -> int:
    failures: list[str] = []

    def expect(name: str, cond: bool) -> None:
        if not cond:
            failures.append(name)

    # _pair_id_from_eui64 zeros the redundancy mid bytes
    expect(
        "pair_id primary",
        _pair_id_from_eui64("02:00:00:00:00:df:d1:96") == "02:00:00:00:00:df:d1:96",
    )
    expect(
        "pair_id redundant",
        _pair_id_from_eui64("02:00:00:00:01:df:d1:96") == "02:00:00:00:00:df:d1:96",
    )

    # _vec_pair_ids: vectorized pair_id derivation matches the scalar version
    senders = np.array(["02:00:00:00:00:df:d1:96", "02:00:00:00:01:df:d1:96"])
    pair_ids = _vec_pair_ids(senders)
    expect(
        "vec_pair_ids primary == scalar", pair_ids[0] == _pair_id_from_eui64(senders[0])
    )
    expect(
        "vec_pair_ids redundant collapses to primary pair_id",
        pair_ids[0] == pair_ids[1],
    )

    # End-to-end summarize_csv on a small CSV fixture
    import tempfile

    csv = (
        "rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,sequence,interval_us,role\n"
        "100,80,1000000,02:00:00:00:00:df:d1:96,1,1000,remote_primary\n"
        "200,180,2000000,02:00:00:00:00:df:d1:96,2,1000,remote_primary\n"
        "350,330,3000000,02:00:00:00:01:df:d1:96,3,1000,remote_redundant\n"
        "400,380,80000000,02:00:00:00:00:df:d1:96,4,1000,remote_primary\n"
    )
    with tempfile.TemporaryDirectory() as td:
        csv_path = Path(td) / "test.csv"
        csv_path.write_text(csv)
        out_path = Path(td) / "test.parquet"
        meta = summarize_csv(
            csv_path,
            out_path,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=1_000_000,
            min_bucket_ms=0.000_001,
        )
        df, meta2, hist = load_summary(out_path)
        expect("summarize total_rows", meta.total_rows == 4)
        expect("summarize meta round-trip", meta2.t0_ns == meta.t0_ns)
        expect("summarize total count", int(df["count"].sum()) == 4)
        expect(
            "summarize late count", int(df["late_count"].sum()) == 1
        )  # seq 4 has 80ms > 50ms WCL
        expect("summarize hist shape", hist.shape == (meta.n_buckets, HIST_N_BINS))
        expect("summarize hist totals match count", int(hist.sum()) == 4)

        # Same CSV through the gzip path — should produce an identical
        # summary (single-stream pass 2, same aggregation logic).
        import gzip

        gz_path = Path(td) / "test.csv.gz"
        with gzip.open(gz_path, "wt") as f:
            f.write(csv)
        gz_out = Path(td) / "test_gz.parquet"
        meta_gz = summarize_csv(
            gz_path,
            gz_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=1_000_000,
            min_bucket_ms=0.000_001,
        )
        df_gz, _, hist_gz = load_summary(gz_out)
        expect("summarize gz total_rows", meta_gz.total_rows == 4)
        expect("summarize gz total count", int(df_gz["count"].sum()) == 4)
        expect("summarize gz late count", int(df_gz["late_count"].sum()) == 1)
        expect(
            "summarize gz matches plain count",
            np.array_equal(df["count"].to_numpy(), df_gz["count"].to_numpy()),
        )
        expect("summarize gz matches plain hist", np.array_equal(hist, hist_gz))

        # trim_seconds: with a 10-second synthetic span and trim=2s, rows
        # at 0s and 10s should be discarded, rows at 2.5s / 5s / 7.5s kept.
        rows = [
            "rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,"
            "sequence,interval_us,role"
        ]
        sender = "02:00:00:00:00:df:d1:96"
        for i, t_s in enumerate([0.0, 2.5, 5.0, 7.5, 10.0]):
            rx = int(t_s * 1_000_000_000)
            rows.append(f"{rx},{rx - 20},1000000,{sender},{i + 1},1000,remote_primary")
        trim_csv = "\n".join(rows) + "\n"
        trim_in = Path(td) / "test_trim.csv"
        trim_in.write_text(trim_csv)
        trim_out = Path(td) / "test_trim.parquet"
        meta_trim = summarize_csv(
            trim_in,
            trim_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=1_000_000,
            min_bucket_ms=0.001,
            trim_seconds=2.0,
        )
        df_trim, _, _ = load_summary(trim_out)
        expect("trim drops first and last rows", int(df_trim["count"].sum()) == 3)
        expect("trim shifts t0_ns forward by trim_ns", meta_trim.t0_ns == 2_000_000_000)
        expect("trim shifts t1_ns back by trim_ns", meta_trim.t1_ns == 8_000_000_000)

        # trim_seconds=0 disables: all 5 rows kept.
        notrim_out = Path(td) / "test_notrim.parquet"
        summarize_csv(
            trim_in,
            notrim_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=1_000_000,
            min_bucket_ms=0.001,
            trim_seconds=0.0,
        )
        df_notrim, _, _ = load_summary(notrim_out)
        expect("trim=0 keeps all rows", int(df_notrim["count"].sum()) == 5)

        # trim larger than half the span: fallback to no trim, all rows kept.
        big_out = Path(td) / "test_bigtrim.parquet"
        summarize_csv(
            trim_in,
            big_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=1_000_000,
            min_bucket_ms=0.001,
            trim_seconds=100.0,
        )
        df_big, _, _ = load_summary(big_out)
        expect("over-trim falls back to no trim", int(df_big["count"].sum()) == 5)

        # Regression: a seq whose primary and redundant copies straddle
        # the trim-window edge must NOT be miscounted as `recovered`.
        # The two copies of a packet arrive a few ms apart; per-pair
        # recovered / true_loss tracking spans the full capture so the
        # trimmed-off copy still counts. Only a seq with genuinely no
        # primary copy anywhere is a real recovered.
        prim_sender = "02:00:00:00:00:df:d1:96"
        redu_sender = "02:00:00:00:01:df:d1:96"
        straddle_rows = [
            "rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,"
            "sequence,interval_us,role"
        ]

        def _owlm_row(
            rx: int, seq: int, sender: str, role: str, lat: int = 1_000_000
        ) -> str:
            return f"{rx},{rx - 20},{lat},{sender},{seq},1000,{role}"

        # (seq, primary_rx_ns, redundant_rx_ns); primary_rx None = the
        # primary copy never arrived. t0 is 0 and trim=2s, so the window
        # is [2.0s, 7.005s]; seq 2's primary lands just before the edge
        # and its redundant just after.
        straddle_plan = [
            (1, 0, 5_000_000),
            (2, 1_999_000_000, 2_001_000_000),  # straddles window_t0
            (3, 3_000_000_000, 3_005_000_000),
            (4, 5_000_000_000, 5_005_000_000),
            (5, None, 6_000_000_000),  # genuine recovered: no primary
            (6, 6_500_000_000, 6_505_000_000),
            (7, 9_000_000_000, 9_005_000_000),
        ]
        for seq, p_rx, r_rx in straddle_plan:
            if p_rx is not None:
                straddle_rows.append(
                    _owlm_row(p_rx, seq, prim_sender, "remote_primary")
                )
            straddle_rows.append(_owlm_row(r_rx, seq, redu_sender, "remote_redundant"))
        straddle_in = Path(td) / "test_straddle.csv"
        straddle_in.write_text("\n".join(straddle_rows) + "\n")
        straddle_out = Path(td) / "test_straddle.parquet"
        summarize_csv(
            straddle_in,
            straddle_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=1_000_000,
            min_bucket_ms=0.001,
            trim_seconds=2.0,
        )
        df_str, _, _ = load_summary(straddle_out)
        expect(
            "boundary-straddling primary not miscounted as recovered",
            int(df_str["recovered_count"].sum()) == 1,
        )
        expect(
            "no false true_loss at trim boundary",
            int(df_str["true_loss_count"].sum()) == 0,
        )

        # Latency-aware classification: a sequence counts as delivered
        # only if at least one copy arrived within WCL. Exercise every
        # primary x redundant combination of in-time / late / absent,
        # plus a never-transmitted gap and a multi-copy best-of pick.
        lat_rows = [
            "rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,"
            "sequence,interval_us,role"
        ]
        in_time_ns = 5_000_000  # 5 ms  — within the 25 ms WCL below
        late_ns = 40_000_000  # 40 ms — past WCL
        # (seq, primary latency | None=absent, redundant latency | None).
        # seq 6 is deliberately omitted: a never-transmitted gap inside
        # the [1, 8] seq range, which must classify as true_loss.
        lat_plan = [
            (1, in_time_ns, None),  # primary in time           -> normal
            (2, None, in_time_ns),  # primary absent, redu OK   -> recovered
            (3, None, late_ns),  # primary absent, redu late -> true_loss
            (4, late_ns, in_time_ns),  # primary late, redu OK     -> recovered
            (5, late_ns, late_ns),  # primary late, redu late   -> true_loss
            (7, in_time_ns, None),  # primary in time           -> normal
        ]
        for seq, p_lat, r_lat in lat_plan:
            rx = seq * 1_000_000_000
            if p_lat is not None:
                lat_rows.append(
                    _owlm_row(rx, seq, prim_sender, "remote_primary", p_lat)
                )
            if r_lat is not None:
                lat_rows.append(
                    _owlm_row(
                        rx + 2_000_000,
                        seq,
                        redu_sender,
                        "remote_redundant",
                        r_lat,
                    )
                )
        # seq 8: two primary copies — one late, one in time. The
        # per-seq scatter-min must keep the in-time copy, leaving the
        # sequence normal. If min-selection broke, seq 8 would become a
        # true_loss and the count below would be 4 instead of 3.
        lat_rows.append(
            _owlm_row(8_000_000_000, 8, prim_sender, "remote_primary", late_ns)
        )
        lat_rows.append(
            _owlm_row(8_001_000_000, 8, prim_sender, "remote_primary", in_time_ns)
        )
        lat_in = Path(td) / "test_latency_loss.csv"
        lat_in.write_text("\n".join(lat_rows) + "\n")
        lat_out = Path(td) / "test_latency_loss.parquet"
        summarize_csv(
            lat_in,
            lat_out,
            plot_width=4,
            wcl_ms=25.0,
            chunksize=1_000_000,
            min_bucket_ms=0.001,
            trim_seconds=0.0,
        )
        df_lat, _, _ = load_summary(lat_out)
        expect(
            "latency-aware: recovered counts late-primary rescue (seq 2,4)",
            int(df_lat["recovered_count"].sum()) == 2,
        )
        expect(
            "latency-aware: true_loss counts late-only seqs and the gap (3,5,6)",
            int(df_lat["true_loss_count"].sum()) == 3,
        )

        # gaps: consecutive true-loss sequences grouped into bursts, each
        # burst's outage measured from its bounding delivered packets.
        # seq 3,4,5 form one burst (3 absent, 4 all-late, 5 absent); seq 9
        # is a lone never-transmitted hole. 1,2,6,7,8,10 are delivered.
        gap_rows = [
            "rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,"
            "sequence,interval_us,role"
        ]
        gap_plan = [
            (1, in_time_ns),
            (2, in_time_ns),
            (4, late_ns),  # seen but late -> still a drop
            (6, in_time_ns),
            (7, in_time_ns),
            (8, in_time_ns),
            (10, in_time_ns),
        ]
        for seq, p_lat in gap_plan:
            rx = seq * 1_000_000_000
            gap_rows.append(_owlm_row(rx, seq, prim_sender, "remote_primary", p_lat))
        gap_in = Path(td) / "test_gaps.csv"
        gap_in.write_text("\n".join(gap_rows) + "\n")
        gdf = compute_gaps(gap_in, wcl_ms=25.0, chunksize=1_000_000, n_workers=1)
        expect("gaps: two drop bursts detected", len(gdf) == 2)
        expect(
            "gaps: total dropped packets = 4 (seq 3,4,5,9)",
            int(gdf["dropped_count"].sum()) == 4,
        )
        burst_a = gdf[gdf["start_seq"] == 3]
        expect(
            "gaps: burst A spans seq 3..5 (3 dropped)",
            not burst_a.empty
            and int(burst_a.iloc[0]["end_seq"]) == 5
            and int(burst_a.iloc[0]["dropped_count"]) == 3,
        )
        expect(
            "gaps: burst A duration = rx(seq6) - rx(seq2) = 4000 ms",
            not burst_a.empty
            and abs(float(burst_a.iloc[0]["duration_ms"]) - 4000.0) < 1e-6,
        )
        expect(
            "gaps: burst A start_t_ns = rx(seq2), duration_ns spans to rx(seq6)",
            not burst_a.empty
            and int(burst_a.iloc[0]["start_t_ns"]) == 2_000_000_000
            and int(burst_a.iloc[0]["duration_ns"]) == 4_000_000_000,
        )
        burst_b = gdf[gdf["start_seq"] == 9]
        expect(
            "gaps: burst B is the lone seq-9 hole, duration = 2000 ms",
            not burst_b.empty
            and int(burst_b.iloc[0]["dropped_count"]) == 1
            and abs(float(burst_b.iloc[0]["duration_ms"]) - 2000.0) < 1e-6,
        )

        # gaps: a sequence discontinuity (talker restart / reset) must NOT
        # be reported as a giant phantom drop burst. Here seq jumps 2 -> 1000
        # but the higher-seq bounding packet (seq 1000 @ 3s) arrived BEFORE
        # the lower-seq one (seq 2 @ 5s), so the missing 3..999 range is out
        # of time order: not real loss. It must be skipped and counted as a
        # discontinuity, leaving zero drop bursts.
        disc_rows = [
            "rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,"
            "sequence,interval_us,role"
        ]
        for seq, rx in [(1, 1), (2, 5), (1000, 3), (1001, 6)]:
            disc_rows.append(
                _owlm_row(
                    rx * 1_000_000_000, seq, prim_sender, "remote_primary", in_time_ns
                )
            )
        disc_in = Path(td) / "test_disc.csv"
        disc_in.write_text("\n".join(disc_rows) + "\n")
        ddf = compute_gaps(disc_in, wcl_ms=25.0, chunksize=1_000_000, n_workers=1)
        expect("gaps: sequence discontinuity yields no drop bursts", len(ddf) == 0)
        expect(
            "gaps: discontinuity is counted, not silently dropped",
            int(ddf.attrs.get("n_discontinuities", 0)) == 1,
        )

        # Synthetic .colbin: hand-build a tiny file matching the C++
        # writer's header layout, then summarize it via the same code
        # path the CLI uses.
        import struct

        cb_path = Path(td) / "test.colbin"
        schema_text = (
            b"rx_gptp_ns:i64,presentation_time_ns:i64,latency_ns:i64,"
            b"sender_eui64:u64,sequence:u32,interval_us:u32,role:u8"
        )
        row_size = 48
        header_total = 64 + ((len(schema_text) + (64 - 1)) // 64) * 64  # round up
        # Match the C++ writer: 64-B preamble, schema text, zero pad.
        preamble = (
            COLBIN_MAGIC
            + struct.pack("<I", COLBIN_VERSION)
            + struct.pack("<I", COLBIN_ENDIAN_MARKER)
            + struct.pack("<I", header_total)
            + struct.pack("<I", row_size)
            + struct.pack("<Q", 3)  # committed_rows
            + struct.pack("<I", len(schema_text))
            + bytes(64 - 8 - 4 - 4 - 4 - 4 - 8 - 4)
        )
        rows = b""
        for i, (rx_ns, lat_ns) in enumerate(
            [
                (1_000_000_000, 1_000_000),
                (2_000_000_000, 2_000_000),
                (3_000_000_000, 60_000_000),  # exceeds 50ms WCL
            ]
        ):
            rows += struct.pack(
                "<qqqQIIB7x",  # i64 i64 i64 u64 u32 u32 u8 + 7 pad
                rx_ns,
                rx_ns - 20,
                lat_ns,
                0x123456789ABCDEF0,  # sender_eui64
                i,
                1000,
                3,  # role=RemotePrimary
            )
        with open(cb_path, "wb") as f:
            f.write(preamble)
            f.write(schema_text)
            f.write(bytes(header_total - len(preamble) - len(schema_text)))
            f.write(rows)

        cb_out = Path(td) / "test_colbin.parquet"
        meta_cb = summarize_csv(
            cb_path,
            cb_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=1_000_000,
            min_bucket_ms=0.000_001,
            trim_seconds=0.0,
        )
        df_cb, _, _ = load_summary(cb_out)
        expect("colbin summarize total count", int(df_cb["count"].sum()) == 3)
        expect("colbin summarize total_rows", meta_cb.total_rows == 3)
        expect("colbin summarize late count", int(df_cb["late_count"].sum()) == 1)

        # Truncated CSV (last row cut mid-line) — should warn and treat
        # the good prefix as the whole input rather than crashing. We
        # use chunksize=4 so the bad row lands in its own chunk and the
        # 4 good rows are preserved; with a larger chunksize the whole
        # chunk would be lost (the realistic loss is one chunk of rows).
        trunc_path = Path(td) / "test_trunc.csv"
        trunc_path.write_text(csv + "500,480,4000000,02:00:00:00:00:df:d")
        trunc_out = Path(td) / "test_trunc.parquet"
        meta_t = summarize_csv(
            trunc_path,
            trunc_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=4,
            min_bucket_ms=0.000_001,
        )
        df_t, _, _ = load_summary(trunc_out)
        expect(
            "truncated CSV summarized without raising", int(df_t["count"].sum()) == 4
        )
        expect("truncated CSV total_rows matches good prefix", meta_t.total_rows == 4)

        # Truncated gzip CSV — same expectation through the streaming path.
        trunc_gz = Path(td) / "test_trunc.csv.gz"
        with gzip.open(trunc_gz, "wt") as f:
            f.write(csv + "500,480,4000000,02:00:00:00:00:df:d")
        trunc_gz_out = Path(td) / "test_trunc_gz.parquet"
        meta_tg = summarize_csv(
            trunc_gz,
            trunc_gz_out,
            plot_width=4,
            wcl_ms=50.0,
            chunksize=4,
            min_bucket_ms=0.000_001,
        )
        df_tg, _, _ = load_summary(trunc_gz_out)
        expect(
            "truncated .gz summarized without raising", int(df_tg["count"].sum()) == 4
        )

    # _hist_bin_indices: log-spaced bins + overflow handling
    arr = np.array([-1_000, 0, 50, 1_000, 1_000_000, 10_000_000_000])
    idx = _hist_bin_indices(arr)
    expect(
        "hist neg <= 0 in bin 0",
        int(idx[0]) == HIST_NEG_BIN and int(idx[1]) == HIST_NEG_BIN,
    )
    expect(
        "hist 50 ns below floor in bin 0",
        int(idx[2]) == HIST_NEG_BIN or int(idx[2]) == 1,
    )  # below floor → ≤0 bucket
    expect("hist 1us in log range", 1 <= int(idx[3]) <= HIST_N_LOG_BINS)
    expect("hist 1ms in log range", 1 <= int(idx[4]) <= HIST_N_LOG_BINS)
    expect("hist 10s in pos overflow", int(idx[5]) == HIST_POS_OVERFLOW_BIN)

    # _bucket_percentiles: p50 from a known histogram
    n_buckets = 2
    h = np.zeros((n_buckets, HIST_N_BINS), dtype=np.int64)
    # Bucket 0: 10 samples all in one log bin near 1 ms
    target_bin = 1 + np.searchsorted(HIST_EDGES_NS, 1_000_000, side="right") - 1
    h[0, target_bin] = 10
    pNN = _bucket_percentiles(h, [50.0, 95.0])
    expect(
        "p50 picks the populated bin",
        HIST_BIN_CENTERS_NS[target_bin - 1] * 0.5
        <= pNN[0, 0]
        <= HIST_BIN_CENTERS_NS[target_bin - 1] * 2.0,
    )
    expect("empty bucket → NaN", np.isnan(pNN[1, 0]))

    # _bucket_percentiles_interp: log-linear interpolation within the chosen bin.
    # With all samples in one bin, p50 = sqrt(edge_lo * edge_hi) (geometric
    # mean) — the bin-center; p10 sits near edge_lo, p90 near edge_hi.
    edge_lo = HIST_EDGES_NS[target_bin - 1]
    edge_hi = HIST_EDGES_NS[target_bin]
    geo_mean = float(np.sqrt(edge_lo * edge_hi))
    pNN_i = _bucket_percentiles_interp(h, [10.0, 50.0, 90.0])
    expect(
        "interp p50 of single-bin population ≈ bin center",
        abs(pNN_i[0, 1] - geo_mean) / geo_mean < 1e-6,
    )
    expect("interp p10 ≥ edge_lo and < geo_mean", edge_lo <= pNN_i[0, 0] < geo_mean)
    expect("interp p90 > geo_mean and ≤ edge_hi", geo_mean < pNN_i[0, 2] <= edge_hi)
    expect("interp empty bucket → NaN", np.isnan(pNN_i[1, 1]))

    # Two adjacent bins, equal counts: p50 must land at the upper edge of
    # the lower bin (= lower edge of the upper bin), and the bin-center
    # variant cannot represent that exactly. Interpolation can.
    h2 = np.zeros((1, HIST_N_BINS), dtype=np.int64)
    h2[0, target_bin] = 100
    h2[0, target_bin + 1] = 100
    pNN_i2 = _bucket_percentiles_interp(h2, [50.0])
    boundary = float(HIST_EDGES_NS[target_bin])  # shared edge
    expect(
        "interp p50 of two-equal-bin population ≈ shared edge",
        abs(pNN_i2[0, 0] - boundary) / boundary < 1e-6,
    )

    # extract_events groups consecutive outliers and breaks at gaps
    df = pd.DataFrame(
        {
            "rx_gptp_ns": [100, 101, 102, 200, 201, 300],
            "presentation_time_ns": [0] * 6,
            "latency_ns": [
                10_000_000,
                11_000_000,
                12_000_000,
                1_000_000,
                6_000_000,
                9_000_000,
            ],
            "sender_eui64": ["x"] * 6,
            "sequence": [1, 2, 3, 4, 5, 6],
            "interval_us": [1000] * 6,
            "role": pd.Categorical(["remote_primary"] * 6),
        }
    )
    events = extract_events(df, threshold_ns=5_000_000, gap_ns=50)
    expect("event count", len(events) == 3)
    expect("event 1 peak", events["peak_latency_ns"].iloc[0] == 12_000_000)
    expect("event 2 starts at 201", events["start_rx_gptp_ns"].iloc[1] == 201)

    # _tai_ns_to_pdt_str: TAI epoch (ns=0) → 1969-12-31 16:59:23 PDT
    # (1970-01-01 00:00:00 UTC = 1969-12-31 17:00:00 PDT, minus 37s for
    # the TAI-UTC offset).
    expect(
        "tai_ns_to_pdt_str epoch", _tai_ns_to_pdt_str(0) == "1969-12-31 16:59:23 PDT"
    )
    # Round-trip a known POSIX moment: 1778457600 s = 2026-05-11 00:00 UTC
    # → 2026-05-10 17:00:00 PDT. Convert backwards to TAI ns first.
    sample_tai_ns = (1778457600 + TAI_UTC_OFFSET_SEC) * 1_000_000_000
    expect(
        "tai_ns_to_pdt_str 2026-05-11 UTC midnight",
        _tai_ns_to_pdt_str(sample_tai_ns) == "2026-05-10 17:00:00 PDT",
    )

    # CSV round-trip via StringIO (used by `events`)
    csv_io = io.StringIO(
        "rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,sequence,interval_us,role\n"
        "100,90,10,02:00:00:00:00:df:d1:96,1,1000,remote_primary\n"
        "200,180,20,02:00:00:00:00:df:d1:96,2,1000,remote_primary\n"
    )
    df = pd.read_csv(csv_io, dtype=CSV_DTYPES)
    expect("loader rows", len(df) == 2)
    expect("loader role category", df["role"].iloc[0] == "remote_primary")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all inline tests passed")
    return 0


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.test:
        return run_tests()
    if args.cmd is None:
        parser.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
