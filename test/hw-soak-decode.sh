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
# Approach: a shell loop that repeatedly decodes a short sample, exercising
# the full create/decode/destroy lifecycle each iteration. CPU is measured
# on the loop subshell via /proc cutime+cstime (includes child processes).
# This avoids -stream_loop which triggers seek — unsupported by hw decoders
# on some Jetson driver versions.
#
# Environment:
#   SOAK_DURATION   — total wall-clock seconds (default: 60)
#   SOAK_INTERVAL   — sampling interval in seconds (default: 2)
#   SOAK_CPU_MAX    — max allowed average CPU% (default: 80)
#   SOAK_DRIFT_MAX  — max allowed drift: late_avg - early_avg (default: 15)
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-soak-decode on variant: ${variant} ==="

SOAK_DURATION="${SOAK_DURATION:-60}"
SOAK_INTERVAL="${SOAK_INTERVAL:-2}"
SOAK_CPU_MAX="${SOAK_CPU_MAX:-200}"
SOAK_DRIFT_MAX="${SOAK_DRIFT_MAX:-15}"

[ -s "${SAMPLE_H264_720P_LONG}" ] || gen-sample-h264-long "${SAMPLE_H264_720P_LONG}" 10

echo "   duration=${SOAK_DURATION}s interval=${SOAK_INTERVAL}s cpu_max=${SOAK_CPU_MAX}% drift_max=${SOAK_DRIFT_MAX}%"

# Start a decode loop in the background. Each iteration is a full
# create → decode → destroy cycle. The subshell runs until killed.
(
  while true; do
    ffmpeg -hide_banner -loglevel error \
      -c:v h264_nvmpi -i "${SAMPLE_H264_720P_LONG}" \
      -f null - 2>/dev/null || true
  done
) &
LOOP_PID=$!

# Ensure cleanup on exit — kill subshell + its ffmpeg children
cleanup() {
  kill "$LOOP_PID" 2>/dev/null
  # Kill any lingering ffmpeg children of the subshell
  pkill -P "$LOOP_PID" 2>/dev/null || true
  wait "$LOOP_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for first decode to start
sleep 2

csv="/tmp/nvmpi-soak-cpu.csv"
: > "$csv"
samples=0
elapsed=0

CLK_TCK=$(getconf CLK_TCK)

# Read cumulative CPU ticks for the subshell + all waited-for children.
# Fields: $14=utime $15=stime $16=cutime $17=cstime
read_cpu_ticks() {
  if [ -f "/proc/$LOOP_PID/stat" ]; then
    awk '{print $14 + $15 + $16 + $17}' "/proc/$LOOP_PID/stat"
  else
    echo 0
  fi
}

prev_cpu_ticks=$(read_cpu_ticks)
prev_wall=$(date +%s%N)

while [ "$elapsed" -lt "$SOAK_DURATION" ]; do
  sleep "$SOAK_INTERVAL"
  elapsed=$((elapsed + SOAK_INTERVAL))

  if ! kill -0 "$LOOP_PID" 2>/dev/null; then
    echo "FAIL(soak): decode loop exited prematurely at ${elapsed}s."
    exit 1
  fi

  cur_cpu_ticks=$(read_cpu_ticks)
  cur_wall=$(date +%s%N)

  delta_ticks=$((cur_cpu_ticks - prev_cpu_ticks))
  delta_wall_ns=$((cur_wall - prev_wall))

  # CPU% = (delta_ticks / CLK_TCK) / (delta_wall_ns / 1e9) * 100
  if [ "$delta_wall_ns" -gt 0 ]; then
    cpu_pct=$(( (delta_ticks * 100 * 1000000000) / (CLK_TCK * delta_wall_ns) ))
  else
    cpu_pct=0
  fi

  echo "${elapsed},${cpu_pct}" >> "$csv"
  echo "   t=${elapsed}s cpu=${cpu_pct}%"
  samples=$((samples + 1))

  prev_cpu_ticks=$cur_cpu_ticks
  prev_wall=$cur_wall
done

# Stop the decode loop
cleanup
trap - EXIT

if [ "$samples" -lt 4 ]; then
  echo "FAIL(soak): too few samples (${samples}) for analysis."
  exit 1
fi

# Analyze: average, early-window avg (first 25%), late-window avg (last 25%)
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

echo "OK: hw-soak-decode passed on ${variant} (${SOAK_DURATION}s, ${samples} samples)."
