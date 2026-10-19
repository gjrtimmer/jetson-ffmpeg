#!/usr/bin/env bash
# Soak-decode suite: repeated h264_nvmpi decode with CPU monitoring.
# Verifies that CPU usage stays flat over time — guards against the monotonic
# CPU rise reported in upstream Keylost#41 / GitHub #24.
#
# The fix (DevicePoll-based capture wait + CV-based pool backpressure) replaces
# two busy-spin loops in the decoder capture thread. This test confirms:
#   1. Steady-state CPU% stays below a per-core threshold.
#   2. CPU% does not rise monotonically (late samples ≈ early samples).
#
# Approach: a shell loop repeatedly decodes a short sample, exercising the full
# create/decode/destroy lifecycle each iteration. CPU is measured PER ITERATION:
# each decode runs in a fresh bash that prints its children's cumulative
# user+sys via `times` when ffmpeg exits, giving whole-run CPU% = cpu_s / wall_s.
# (The previous metric read cutime/cstime of the loop subshell every 2 s; those
# only advance when a child exits, so samples aliased against iteration ends and
# was noisy: drift 20.3% was seen once on a healthy build.) Early/late windows
# are the first/last quarter of iterations. This avoids -stream_loop, which
# triggers a seek that some Jetson hw decoders crash on.
#
# Environment:
#   SOAK_DURATION   - total wall-clock seconds (default: 30)
#   SOAK_CPU_MAX    - max allowed average CPU% of one core (default: 200)
#   SOAK_DRIFT_MAX  - max allowed drift: late_avg - early_avg (default: 15;
#                     whole-iteration averages over >=4 iterations per window
#                     are far less noisy than the old 2 s samples, and a real
#                     busy-spin regression (GitHub #24) rises well above 15
#                     points, so the threshold is kept)
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-soak-decode on variant: ${variant} ==="

SOAK_DURATION="${SOAK_DURATION:-30}"
SOAK_CPU_MAX="${SOAK_CPU_MAX:-200}"
SOAK_DRIFT_MAX="${SOAK_DRIFT_MAX:-15}"

ensure_sample SAMPLE_H264_720P_LONG gen-sample-h264-long 10

echo "   duration=${SOAK_DURATION}s cpu_max=${SOAK_CPU_MAX}% drift_max=${SOAK_DRIFT_MAX}%"

csv="/tmp/nvmpi-soak-cpu.csv"
stop="/tmp/nvmpi-soak.stop"
: > "$csv"
rm -f "$stop"

# decode_once: one create -> decode -> destroy cycle; appends "t,cpu_pct" to csv.
decode_once() {
  local t0 t1 out cpu_s
  t0=$(date +%s%N)
  out=$(bash -c 'timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error \
      -c:v h264_nvmpi -i "$0" -f null - 2>&1; times' "${SAMPLE_H264_720P_LONG}" 2>&1) || true
  t1=$(date +%s%N)
  # last line of `times` = children: "<u_min>m<u_s>s <s_min>m<s_s>s"
  cpu_s=$(printf '%s\n' "$out" | tail -1 | awk '{
    split($1,u,/[ms]/); split($2,s,/[ms]/); printf "%.3f", u[1]*60+u[2]+s[1]*60+s[2] }')
  awk -v c="${cpu_s:-0}" -v a="$t0" -v b="$t1" -v s="$SOAK_START" \
    'BEGIN{ w=(b-a)/1e9; if (w>0) printf "%d,%.0f\n", (b-s)/1e9, c*100/w }' >> "$csv"
}

SOAK_START=$(date +%s%N)
(
  while [ ! -e "$stop" ]; do decode_once; done
) &
LOOP_PID=$!

cleanup() {
  touch "$stop"
  kill "$LOOP_PID" 2>/dev/null || true
  pkill -P "$LOOP_PID" 2>/dev/null || true
  wait "$LOOP_PID" 2>/dev/null || true
  rm -f "$stop"
}
trap cleanup EXIT

elapsed=0
while [ "$elapsed" -lt "$SOAK_DURATION" ]; do
  sleep 2
  elapsed=$((elapsed + 2))
  if ! kill -0 "$LOOP_PID" 2>/dev/null; then
    echo "FAIL(soak): decode loop exited prematurely at ${elapsed}s."
    exit 1
  fi
  echo "   t=${elapsed}s iterations=$(wc -l < "$csv")"
done

# Let the in-flight iteration finish (stop flag), then analyse.
touch "$stop"
wait "$LOOP_PID" 2>/dev/null || true
trap - EXIT
rm -f "$stop"
samples=$(wc -l < "$csv")

if [ "$samples" -lt 4 ]; then
  echo "FAIL(soak): too few iterations (${samples}) for analysis."
  exit 1
fi

# Analyze: average, early-window avg (first 25% of iterations), late-window avg (last 25%)
analyze() {
  awk -F, -v cpu_max="$SOAK_CPU_MAX" -v drift_max="$SOAK_DRIFT_MAX" '
  {
    cpu[NR] = $2
    total += $2
    n++
  }
  END {
    avg = total / n
    quarter = int(n / 4)
    if (quarter < 1) quarter = 1

    early_sum = 0
    for (i = 1; i <= quarter; i++) early_sum += cpu[i]
    early_avg = early_sum / quarter

    late_sum = 0
    for (i = n - quarter + 1; i <= n; i++) late_sum += cpu[i]
    late_avg = late_sum / quarter

    drift = late_avg - early_avg

    printf "   avg_cpu=%.1f%% early_avg=%.1f%% late_avg=%.1f%% drift=%.1f%%\n", avg, early_avg, late_avg, drift

    failed = 0
    if (avg > cpu_max) {
      printf "FAIL(soak): average CPU %.1f%% exceeds threshold %d%%.\n", avg, cpu_max
      printf "      The decoder capture thread may still be busy-spinning.\n"
      printf "      Check src/nvmpi_dec_capture.cpp DevicePoll() path.\n"
      failed = 1
    }
    if (drift > drift_max) {
      printf "FAIL(soak): CPU drift %.1f%% exceeds threshold %d%%.\n", drift, drift_max
      printf "      CPU usage is rising over time — the original bug (GitHub #24).\n"
      printf "      Check for resource leaks or poll loop regressions.\n"
      failed = 1
    }
    exit failed
  }
  ' "$1"
}

if ! analyze "$csv"; then
  echo "   raw samples: $(cat "$csv")"
  exit 1
fi

echo "OK: hw-soak-decode passed on ${variant} (${SOAK_DURATION}s, ${samples} iterations)."
