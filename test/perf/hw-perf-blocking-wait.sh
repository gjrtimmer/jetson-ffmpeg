#!/usr/bin/env bash
# Performance measurement for blocking wait (issue #10).
# NON-FATAL: reports metrics, does not gate pass/fail.
set -eu
# shellcheck source-path=SCRIPTDIR/..
# shellcheck source=lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-perf-blocking-wait on variant: ${variant} ==="

ensure_sample SAMPLE_H264_720P_LONG gen-sample-h264-long 10

echo ""
echo "== Metric 1: First-frame latency =="
measure_first_frame_latency() {
  local start_ns end_ns
  start_ns=$(date +%s%N)
  run_ff -T 15 -loglevel error "$@" \
    -c:v h264_nvmpi -i "${SAMPLE_H264_720P_LONG}" \
    -frames:v 1 -f null - || true
  end_ns=$(date +%s%N)
  echo $(( (end_ns - start_ns) / 1000000 ))
}
sum_normal=0; sum_lowdelay=0
for _ in 1 2 3; do
  lat_n=$(measure_first_frame_latency)
  lat_l=$(measure_first_frame_latency -flags low_delay)
  sum_normal=$((sum_normal + lat_n))
  sum_lowdelay=$((sum_lowdelay + lat_l))
done
avg_normal=$((sum_normal / 3))
avg_lowdelay=$((sum_lowdelay / 3))
if [ "$avg_lowdelay" -gt 0 ]; then
  improvement=$(echo "$avg_normal $avg_lowdelay" | awk '{printf "%.1fx", $1/$2}')
else
  improvement="N/A"
fi
echo "PERF: first_frame_latency_normal=${avg_normal}ms"
echo "PERF: first_frame_latency_lowdelay=${avg_lowdelay}ms"
echo "PERF: first_frame_improvement=${improvement}"

echo ""
echo "== Metric 2: CPU usage =="
measure_cpu() {
  # Raw timeout on purpose: /usr/bin/time must wrap the ffmpeg process itself.
  local cpu
  cpu=$( { /usr/bin/time -v timeout -k 5 30 ffmpeg -y -hide_banner -loglevel error "$@" \
    -c:v h264_nvmpi -i "${SAMPLE_H264_720P_LONG}" \
    -f null - ; } 2>&1 | grep "Percent of CPU" | grep -oP '[0-9]+' || echo "N/A" )
  echo "$cpu"
}
cpu_normal=$(measure_cpu)
cpu_lowdelay=$(measure_cpu -flags low_delay)
echo "PERF: cpu_percent_normal=${cpu_normal}%"
echo "PERF: cpu_percent_lowdelay=${cpu_lowdelay}%"

echo ""
echo "== Metric 3: Decode throughput (fps) =="
measure_fps() {
  local stderr="/tmp/nvmpi-perf-bw-$1.log"
  shift
  run_ff -T 120 -benchmark "$@" \
    -c:v h264_nvmpi -i "${SAMPLE_H264_720P_LONG}" \
    -f null - || true
  printf '%s\n' "$FF_OUT" > "$stderr"
  local speed
  speed=$(grep -oP 'speed=\s*\K[0-9.]+' "$stderr" | tail -1)
  if [ -n "$speed" ]; then
    echo "$speed 30" | awk '{printf "%.1f", $1 * $2}'
  else
    echo "N/A"
  fi
}
fps_normal=$(measure_fps normal)
fps_lowdelay=$(measure_fps lowdelay -flags low_delay)
echo "PERF: fps_normal=${fps_normal}"
echo "PERF: fps_lowdelay=${fps_lowdelay}"

echo ""
echo "========================================"
echo "  BLOCKING WAIT PERF SUMMARY (${variant})"
echo "========================================"
echo "  First-frame latency: normal=${avg_normal}ms, low_delay=${avg_lowdelay}ms (${improvement})"
echo "  CPU usage:           normal=${cpu_normal}%, low_delay=${cpu_lowdelay}%"
echo "  Throughput:          normal=${fps_normal} fps, low_delay=${fps_lowdelay} fps"
echo ""
echo "OK: hw-perf-blocking-wait completed on ${variant}."
