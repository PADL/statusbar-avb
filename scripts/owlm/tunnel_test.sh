#!/usr/bin/env bash
#
# tunnel_test.sh -- turnkey two-node AVB UDP-tunnel telemetry test.
#
# Runs the statusbar-avb entity on two nodes for a fixed window with the
# per-packet egress colbin recorder enabled on each side, waits out the
# window, pulls both colbins back, and runs owlm_analyze
# (summarize -> join -> plot -> gaps) into a local results directory.
#
# Each node's egress colbin records one row per received tunnel packet, so
# NODE_A's file is the peer->A direction (audio A *receives*) and NODE_B's is
# the A->B direction. WCL (worst-case latency / playout deadline) defaults to
# 22 ms and is used only for the analysis 'late' classification.
#
# Usage:
#   tunnel_test.sh [options] DURATION NODE_A NODE_B [-- EXTRA_ENTITY_ARGS...]
#
#   DURATION   steady-state measurement window in seconds. The entity actually
#              runs for STARTUP_GRACE + SETTLE + DURATION; the startup+settle
#              prefix is captured but trimmed from the analysis so the reported
#              latency/loss reflect steady state, not connect-time transients.
#   NODE_A     ssh host for node A   (e.g. node-a)
#   NODE_B     ssh host for node B   (e.g. node-b-tunnel)
#   EXTRA...   any extra args appended to the entity cmdline on BOTH nodes
#              (e.g. --udptun.redundant --udptun.temporal_shift_ms=5)
#
# Options:
#   -w MS      WCL deadline in ms for analysis        (default 22)
#   -o DIR     local output directory                 (default <repo>/results/avb-tunnel/<tag>)
#   -c PATH    entity config toml on the nodes         (default /etc/statusbar-avb/entity.toml)
#   -A CMD     command to run ON node A after launch   (e.g. ACMP connect; optional)
#   -B CMD     command to run ON node B after launch   (optional)
#   -g SEC     post-connect settle seconds            (default 15) — quiet period
#              after the hooks so the connect-induced talker re-advertise / seq
#              reset lands in the trimmed prefix, not the measured window.
#   -k         keep manual run / do NOT restart the systemd entity afterwards
#
# Env:
#   OWLM_PYTHON   python interpreter with numpy/pandas/pyarrow/matplotlib.
#                 If unset, a venv is auto-created at <repo>/.owlm-venv.
#
# Examples:
#   # 30-min A<->B telemetry test, reconnect both MOTUs after launch:
#   tunnel_test.sh -A 'statusbar-atdecc-ctl --interface=eth0 --command=connect --talker=node-a:0 --listener=node-b:0' \
#                  1800 node-a node-b-tunnel
#
#   # 5-min test with redundancy enabled:
#   tunnel_test.sh 300 node-a node-b-tunnel -- --udptun.redundant --udptun.temporal_shift_ms=5
#
set -euo pipefail

# ---- defaults -------------------------------------------------------------
WCL_MS=22
OUTDIR=""
CONFIG="/etc/statusbar-avb/entity.toml"
HOOK_A=""
HOOK_B=""
KEEP=0
BIN="/usr/local/bin/statusbar-avb-audio-io"
STARTUP_GRACE=12      # seconds to wait for gPTP lock + tunnel hole-punch
SETTLE=15             # seconds to let streams settle AFTER the connect hooks,
                      # before the measured window — the ACMP/MSRP connect makes
                      # the talker re-advertise and reset its sequence counter,
                      # so this prefix is captured but trimmed from the analysis.
END_SLACK=20          # seconds of slack added after the window before collecting

die() { echo "error: $*" >&2; exit 1; }
log() { echo ">> $*" >&2; }

# ---- arg parsing ----------------------------------------------------------
while getopts ":w:o:c:A:B:g:k" opt; do
  case "$opt" in
    w) WCL_MS="$OPTARG" ;;
    o) OUTDIR="$OPTARG" ;;
    c) CONFIG="$OPTARG" ;;
    A) HOOK_A="$OPTARG" ;;
    B) HOOK_B="$OPTARG" ;;
    g) SETTLE="$OPTARG" ;;
    k) KEEP=1 ;;
    \?) die "unknown option -$OPTARG" ;;
    :) die "option -$OPTARG needs an argument" ;;
  esac
done
shift $((OPTIND - 1))

[ $# -ge 3 ] || die "usage: $0 [options] DURATION NODE_A NODE_B [-- EXTRA_ENTITY_ARGS...]"
DURATION="$1"; NODE_A="$2"; NODE_B="$3"; shift 3
[ "${1:-}" = "--" ] && shift || true
EXTRA=("$@")
[[ "$DURATION" =~ ^[0-9]+$ ]] || die "DURATION must be an integer number of seconds"
[[ "$SETTLE" =~ ^[0-9]+$ ]] || die "settle (-g) must be an integer number of seconds"

# Entity runs long enough to give DURATION of steady data after startup+settle;
# the startup+settle prefix is trimmed from the analysis (both ends, symmetric).
RUN_SECS=$(( STARTUP_GRACE + SETTLE + DURATION ))
TRIM=$(( STARTUP_GRACE + SETTLE ))

# ---- paths ----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"     # avb/scripts/owlm -> repo root
ANALYZE="$SCRIPT_DIR/../../python/owlm/owlm_analyze.py"
[ -f "$ANALYZE" ] || die "owlm_analyze.py not found at $ANALYZE"

TAG="$(date -u +%Y%m%dT%H%M%SZ)"
[ -n "$OUTDIR" ] || OUTDIR="$REPO_ROOT/results/avb-tunnel/$TAG"
mkdir -p "$OUTDIR"

# colbin sizing: 48 B/row * ~2.2k pkt/s ~= 0.1 MB/s; redundancy doubles it.
# Use 0.25 MB/s + 32 MB headroom so a redundant run is covered too. Size for
# the full RUN_SECS (startup+settle+duration), not just the measured window.
MAX_MB=$(( (RUN_SECS * 25 / 100) + 32 ))

REMOTE_COLBIN_A="/var/tmp/${TAG}-A.colbin"
REMOTE_COLBIN_B="/var/tmp/${TAG}-B.colbin"
REMOTE_LOG_A="/var/tmp/${TAG}-A.log"
REMOTE_LOG_B="/var/tmp/${TAG}-B.log"

# bracket-trick pattern: matches the running entity but NOT this pgrep itself.
ALIVE_PAT='[s]tatusbar-avb-audio-io --config-load'

log "tag=$TAG  duration=${DURATION}s (run=${RUN_SECS}s, grace=${STARTUP_GRACE}s settle=${SETTLE}s trim=${TRIM}s)  wcl=${WCL_MS}ms  colbin_max=${MAX_MB}MB"
log "node A=$NODE_A  node B=$NODE_B"
[ ${#EXTRA[@]} -gt 0 ] && log "extra entity args: ${EXTRA[*]}"
log "output -> $OUTDIR"

# ---- per-node launch ------------------------------------------------------
# $1 host  $2 colbin  $3 log  -> echoes "was-active" or "was-inactive"
launch_node() {
  local host="$1" colbin="$2" logf="$3"
  local was
  was="$(ssh -o ConnectTimeout=10 "$host" 'systemctl is-active statusbar-avb-entity 2>/dev/null || true')"
  ssh -o ConnectTimeout=10 "$host" "sudo systemctl stop statusbar-avb-entity 2>/dev/null || true; sleep 1; \
    sudo bash -c 'setsid timeout --signal=INT ${RUN_SECS} ${BIN} \
      --config-load=${CONFIG} \
      --udptun.egress_colbin=${colbin} --udptun.egress_colbin_max_mb=${MAX_MB} \
      ${EXTRA[*]:-} </dev/null >${logf} 2>&1 &'" >&2
  echo "$was"
}

log "launching capture on both nodes..."
WAS_A="$(launch_node "$NODE_A" "$REMOTE_COLBIN_A" "$REMOTE_LOG_A")"
WAS_B="$(launch_node "$NODE_B" "$REMOTE_COLBIN_B" "$REMOTE_LOG_B")"
START_EPOCH="$(date +%s)"

log "waiting ${STARTUP_GRACE}s for gPTP lock + tunnel hole-punch..."
sleep "$STARTUP_GRACE"

# ---- optional post-launch hooks (ACMP connects, etc.) ---------------------
if [ -n "$HOOK_A" ]; then log "hook on $NODE_A: $HOOK_A"; ssh "$NODE_A" "$HOOK_A" >&2 || log "hook A returned non-zero"; fi
if [ -n "$HOOK_B" ]; then log "hook on $NODE_B: $HOOK_B"; ssh "$NODE_B" "$HOOK_B" >&2 || log "hook B returned non-zero"; fi

# ---- early bidirectional-RX gate -----------------------------------------
# The egress only records packets it RECEIVES. If the tunnel is unidirectional
# at this launch (a hole-punch miss), the colbin stays empty -- abort now
# rather than waste the whole window.
rx_rate() { ssh -o ConnectTimeout=10 "$1" 'sudo timeout 2 tcpdump -ni eth0 "udp port 17220 and inbound" 2>/dev/null | wc -l'; }
log "checking inbound tunnel RX on both nodes..."
RXA="$(rx_rate "$NODE_A")"; RXB="$(rx_rate "$NODE_B")"
log "inbound/2s:  $NODE_A=$RXA  $NODE_B=$RXB"
if [ "${RXA:-0}" -lt 10 ] || [ "${RXB:-0}" -lt 10 ]; then
  log "!! tunnel is not bidirectional (one side receives nothing) -- aborting."
  ssh "$NODE_A" "sudo pkill -INT -f '$ALIVE_PAT' || true" >&2 || true
  ssh "$NODE_B" "sudo pkill -INT -f '$ALIVE_PAT' || true" >&2 || true
  [ "$KEEP" -eq 1 ] || { [ "$WAS_A" = active ] && ssh "$NODE_A" 'sudo systemctl start statusbar-avb-entity' >&2 || true; [ "$WAS_B" = active ] && ssh "$NODE_B" 'sudo systemctl start statusbar-avb-entity' >&2 || true; }
  die "unidirectional tunnel; nothing captured"
fi

# ---- post-connect settle --------------------------------------------------
# The connect hooks make the talker re-advertise and reset its sequence
# counter ~immediately. Let that finish during the settle so the steady-state
# measured window is clean; the settle (+ startup grace) is trimmed from the
# analysis below, and the discontinuity filter catches any reset that slips in.
if [ "$SETTLE" -gt 0 ]; then
  log "settling ${SETTLE}s post-connect before the ${DURATION}s measured window..."
  sleep "$SETTLE"
fi

# ---- wait out the window --------------------------------------------------
REMAIN=$(( START_EPOCH + RUN_SECS + END_SLACK - $(date +%s) ))
[ "$REMAIN" -gt 0 ] || REMAIN=0
log "measuring for ~${DURATION}s (~${REMAIN}s until collect)..."
sleep "$REMAIN"

# confirm both processes have exited (timeout enforces the stop)
for host in "$NODE_A" "$NODE_B"; do
  for _ in $(seq 1 30); do
    n="$(ssh -o ConnectTimeout=10 "$host" "pgrep -f '$ALIVE_PAT' | wc -l" 2>/dev/null || echo 1)"
    [ "$n" = "0" ] && break
    sleep 2
  done
done
log "capture finished."

# ---- collect --------------------------------------------------------------
# colbins are root-owned (run via sudo); gzip+chown then pull. Trailing zeros
# of the pre-allocated map are already truncated away on clean exit.
fetch() {
  local host="$1" remote="$2" local_gz="$3"
  ssh "$host" "sudo gzip -f '$remote' && sudo chown \$USER '${remote}.gz'" >&2
  scp -q "$host:${remote}.gz" "$local_gz"
  gunzip -f "$local_gz"
  ssh "$host" "sudo rm -f '${remote}.gz'" >&2 || true
}
log "pulling colbins..."
fetch "$NODE_A" "$REMOTE_COLBIN_A" "$OUTDIR/${NODE_A}.colbin.gz"
fetch "$NODE_B" "$REMOTE_COLBIN_B" "$OUTDIR/${NODE_B}.colbin.gz"
scp -q "$NODE_A:$REMOTE_LOG_A" "$OUTDIR/${NODE_A}.log" || true
scp -q "$NODE_B:$REMOTE_LOG_B" "$OUTDIR/${NODE_B}.log" || true

# ---- restore production ---------------------------------------------------
if [ "$KEEP" -eq 0 ]; then
  [ "$WAS_A" = active ] && { log "restarting entity on $NODE_A"; ssh "$NODE_A" 'sudo systemctl start statusbar-avb-entity' >&2 || true; }
  [ "$WAS_B" = active ] && { log "restarting entity on $NODE_B"; ssh "$NODE_B" 'sudo systemctl start statusbar-avb-entity' >&2 || true; }
  [ -n "$HOOK_A" ] && ssh "$NODE_A" "$HOOK_A" >&2 || true
  [ -n "$HOOK_B" ] && ssh "$NODE_B" "$HOOK_B" >&2 || true
fi

# ---- pick / build an analysis python --------------------------------------
pick_python() {
  if [ -n "${OWLM_PYTHON:-}" ] && "$OWLM_PYTHON" -c 'import numpy,pandas,pyarrow,matplotlib' 2>/dev/null; then echo "$OWLM_PYTHON"; return; fi
  for p in "$REPO_ROOT/.owlm-venv/bin/python" /tmp/owlm-venv/bin/python python3; do
    "$p" -c 'import numpy,pandas,pyarrow,matplotlib' 2>/dev/null && { echo "$p"; return; }
  done
  log "creating analysis venv at $REPO_ROOT/.owlm-venv ..."
  python3 -m venv "$REPO_ROOT/.owlm-venv" >&2
  "$REPO_ROOT/.owlm-venv/bin/pip" -q install numpy pandas pyarrow matplotlib >&2
  echo "$REPO_ROOT/.owlm-venv/bin/python"
}
PY="$(pick_python)"
log "analysis python: $PY"

# ---- analyze --------------------------------------------------------------
CA="$OUTDIR/${NODE_A}.colbin"; CB="$OUTDIR/${NODE_B}.colbin"
SA="$OUTDIR/${NODE_A}-summary.parquet"; SB="$OUTDIR/${NODE_B}-summary.parquet"
# --trim-seconds drops the startup+settle prefix (and an equal tail) so the
# latency/loss summary reflects steady state, not the connect-time transient.
log "summarize (trim ${TRIM}s/end)..."
"$PY" "$ANALYZE" summarize "$CA" -o "$SA" --worst-case-latency-ms "$WCL_MS" --trim-seconds "$TRIM"
"$PY" "$ANALYZE" summarize "$CB" -o "$SB" --worst-case-latency-ms "$WCL_MS" --trim-seconds "$TRIM"
log "join..."
"$PY" "$ANALYZE" join "$SA" "$SB" | tee "$OUTDIR/join.txt"
log "plot..."
"$PY" "$ANALYZE" plot "$SA" "$SB" -o "$OUTDIR/report.pdf" \
  --title "AVB tunnel ${NODE_A}<->${NODE_B}  ${DURATION}s  ${TAG}" \
  --label-a "rx@${NODE_A}" --label-b "rx@${NODE_B}" || log "plot failed (non-fatal)"
log "gaps..."
"$PY" "$ANALYZE" gaps "$CA" --worst-case-latency-ms "$WCL_MS" | tee "$OUTDIR/${NODE_A}-gaps.txt" || true
"$PY" "$ANALYZE" gaps "$CB" --worst-case-latency-ms "$WCL_MS" | tee "$OUTDIR/${NODE_B}-gaps.txt" || true

# ---- run-info -------------------------------------------------------------
{
  echo "tag=$TAG"
  echo "started_utc=$(date -u -r "$START_EPOCH" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "duration_s=$DURATION  run_s=$RUN_SECS  startup_grace_s=$STARTUP_GRACE  settle_s=$SETTLE  trim_s=$TRIM"
  echo "wcl_ms=$WCL_MS  colbin_max_mb=$MAX_MB"
  echo "node_a=$NODE_A (rx = peer->A)   node_b=$NODE_B (rx = A->peer)"
  echo "config=$CONFIG"
  echo "extra_args=${EXTRA[*]:-}"
  echo "early_inbound_2s: $NODE_A=$RXA  $NODE_B=$RXB"
} > "$OUTDIR/run-info.txt"

log "DONE.  results in: $OUTDIR"
echo "$OUTDIR"
