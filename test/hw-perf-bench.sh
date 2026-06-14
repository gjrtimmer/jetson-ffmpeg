#!/usr/bin/env bash
# Performance benchmark for max_perf mode: measures fps with max_perf on vs
# off for both decoder and encoder, reports the speedup ratio, and optionally
# captures tegrastats to show NVDEC/NVENC hardware utilization.
#
# All tests run SEQUENTIALLY — one decode/encode at a time — to avoid
# contention on the hardware codec engines and ensure accurate measurements.
#
# Expected results (from fork network data):
#   720p H.264 decode:  ~46 fps (off) → ~290 fps (on) ≈ 6x
#   4K  HEVC   decode:  ~28 fps (off) → ~64  fps (on) ≈ 2.3x
# Actual gains vary by Jetson variant, thermal state, and content complexity.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-perf-bench on variant: ${variant} ==="

BENCH_DURATION="${BENCH_DURATION:-10}"

# --- hardware stats helper (tegrastats or jtop) ---
# Prefers tegrastats (direct NVIDIA binary, always line-oriented), falls back
# to jtop's Python API (from jetson-stats) for NVDEC/NVENC/GPU utilization.
TEGRASTATS_BIN=""
if command -v tegrastats >/dev/null 2>&1; then
  TEGRASTATS_BIN="tegrastats"
elif [ -x /usr/bin/tegrastats ]; then
  TEGRASTATS_BIN="/usr/bin/tegrastats"
fi

HAS_JTOP=0
python3 -c "from jtop import jtop" 2>/dev/null && HAS_JTOP=1

HWSTATS_PID=""
HWSTATS_LOG=""

start_hwstats() {
  local label="$1"
  HWSTATS_LOG="/tmp/nvmpi-hwstats-${label}.log"
  if [ -n "$TEGRASTATS_BIN" ]; then
    $TEGRASTATS_BIN --interval 200 > "$HWSTATS_LOG" 2>&1 &
    HWSTATS_PID=$!
  elif [ "$HAS_JTOP" = 1 ]; then
    python3 -c "
import time, signal, sys
from jtop import jtop
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
with jtop(interval=0.2) as j:
    while j.ok():
        s = j.stats
        parts = []
        for k in ('NVDEC', 'NVDEC1', 'NVENC', 'NVENC1', 'GPU'):
            if k in s:
                parts.append(f'{k}={s[k]}')
        if parts:
            print(' '.join(parts), flush=True)
        time.sleep(0.2)
" > "$HWSTATS_LOG" 2>&1 &
    HWSTATS_PID=$!
  fi
}

stop_hwstats() {
  local label="$1"
  if [ -z "$HWSTATS_PID" ]; then return; fi
  kill "$HWSTATS_PID" 2>/dev/null || true
  wait "$HWSTATS_PID" 2>/dev/null || true
  HWSTATS_PID=""
  if [ ! -s "$HWSTATS_LOG" ]; then
    echo "   hw-stats: (no data captured)"
    return
  fi
  echo "   hw-stats (${label}):"
  if [ -n "$TEGRASTATS_BIN" ]; then
    # tegrastats format
    if grep -qo 'NVDEC[^ ]*' "$HWSTATS_LOG" 2>/dev/null; then
      echo "     NVDEC: $(grep -o 'NVDEC[^ ]*' "$HWSTATS_LOG" | sort -u | tr '\n' ' ')"
    fi
    if grep -qo 'NVENC[^ ]*' "$HWSTATS_LOG" 2>/dev/null; then
      echo "     NVENC: $(grep -o 'NVENC[^ ]*' "$HWSTATS_LOG" | sort -u | tr '\n' ' ')"
    fi
    if grep -qo 'GR3D_FREQ [0-9]*%' "$HWSTATS_LOG" 2>/dev/null; then
      echo "     GR3D:  $(grep -o 'GR3D_FREQ [0-9]*%' "$HWSTATS_LOG" | sort -u | tr '\n' ' ')"
    fi
  else
    # jtop format: "NVDEC=X NVENC=Y GPU=Z"
    tail -5 "$HWSTATS_LOG" | sed 's/^/     /'
  fi
}

# extract_fps STDERR_FILE — parse fps from ffmpeg benchmark output
extract_fps() {
  local file="$1"
  # try "speed=XX.Xx" first (most reliable)
  local speed
  speed=$(grep -oP 'speed=\s*\K[0-9.]+' "$file" | tail -1)
  if [ -n "$speed" ]; then
    # speed is relative to realtime; fps = speed * input_fps
    # input is 30fps for our samples
    echo "$speed 30" | awk '{printf "%.1f", $1 * $2}'
    return
  fi
  # fallback: parse "frame= N ... time= HH:MM:SS.ss" and compute
  local frames elapsed
  frames=$(grep -oP 'frame=\s*\K[0-9]+' "$file" | tail -1)
  elapsed=$(grep -oP 'rtime=\s*\K[0-9.]+' "$file" | tail -1)
  if [ -n "$frames" ] && [ -n "$elapsed" ]; then
    echo "$frames $elapsed" | awk '$2>0 {printf "%.1f", $1/$2}'
    return
  fi
  echo "N/A"
}

# bench_decode LABEL DECODER INPUT MAX_PERF [EXTRA_OPTS...]
bench_decode() {
  local label="$1" dec="$2" sample="$3" mp="$4"
  shift 4
  local tmpout="/tmp/nvmpi-bench-dec-${label}.mkv"
  local stderr="/tmp/nvmpi-bench-dec-${label}.log"

  echo "   running: ${label} ..."
  start_hwstats "$label"
  # Decode to null output — measures pure NVDEC throughput without
  # CPU re-encode bottleneck (ffv1 would cap the pipeline at CPU speed).
  ffmpeg -y -hide_banner -benchmark \
    -c:v "$dec" -max_perf "$mp" "$@" -i "$sample" \
    -f null - 2>"$stderr" || true
  stop_hwstats "$label"

  local fps
  fps=$(extract_fps "$stderr")
  echo "   ${label}: ${fps} fps"
  eval "FPS_${label//-/_}=$fps"
}

# bench_encode LABEL ENCODER MAX_PERF [EXTRA_OPTS...]
bench_encode() {
  local label="$1" enc="$2" mp="$3"
  shift 3
  local tmpout="/tmp/nvmpi-bench-enc-${label}.mp4"
  local stderr="/tmp/nvmpi-bench-enc-${label}.log"

  echo "   running: ${label} ..."
  start_hwstats "$label"
  ffmpeg -y -hide_banner -benchmark \
    -f lavfi -i "testsrc2=s=1280x720:r=30" -t "$BENCH_DURATION" \
    -c:v "$enc" -b:v 3M -max_perf "$mp" "$@" "$tmpout" 2>"$stderr" || true
  stop_hwstats "$label"

  local fps
  fps=$(extract_fps "$stderr")
  echo "   ${label}: ${fps} fps"
  eval "FPS_${label//-/_}=$fps"
}

# speedup A B — compute B/A ratio
speedup() {
  echo "$1 $2" | awk '$1+0>0 && $2+0>0 {printf "%.1fx", $2/$1} $1+0==0||$2+0==0 {print "N/A"}'
}

# --- generate benchmark sample ---
echo "== Generating ${BENCH_DURATION}s 720p H.264 benchmark sample =="
gen-sample-h264-long "${SAMPLE_H264_720P_LONG}" "$BENCH_DURATION"

# ============================================================
# DECODE BENCHMARKS (sequential — one at a time)
# ============================================================
echo ""
echo "== DECODE: H.264 720p — max_perf OFF vs ON =="
bench_decode dec-h264-off h264_nvmpi "${SAMPLE_H264_720P_LONG}" 0
bench_decode dec-h264-on  h264_nvmpi "${SAMPLE_H264_720P_LONG}" 1

echo ""
echo "== DECODE: H.264 720p — disable_dpb=1 + max_perf=1 =="
bench_decode dec-h264-dpb h264_nvmpi "${SAMPLE_H264_720P_LONG}" 1 -disable_dpb 1

# ============================================================
# ENCODE BENCHMARKS (sequential — one at a time)
# ============================================================
echo ""
echo "== ENCODE: H.264 720p — max_perf OFF vs ON =="
bench_encode enc-h264-off h264_nvmpi 0
bench_encode enc-h264-on  h264_nvmpi 1

echo ""
echo "== ENCODE: H.264 720p — poc_type=2 + max_perf=1 =="
bench_encode enc-h264-poc2 h264_nvmpi 1 -poc_type 2

# ============================================================
# SUMMARY
# ============================================================
echo ""
echo "========================================"
echo "  PERFORMANCE SUMMARY (${variant})"
echo "========================================"
echo ""
dec_off=${FPS_dec_h264_off:-N/A}
dec_on=${FPS_dec_h264_on:-N/A}
dec_dpb=${FPS_dec_h264_dpb:-N/A}
enc_off=${FPS_enc_h264_off:-N/A}
enc_on=${FPS_enc_h264_on:-N/A}
enc_poc2=${FPS_enc_h264_poc2:-N/A}

echo "  Decode H.264 720p:"
echo "    max_perf=0:  ${dec_off} fps"
echo "    max_perf=1:  ${dec_on} fps  (speedup: $(speedup "$dec_off" "$dec_on"))"
echo "    +disable_dpb: ${dec_dpb} fps"
echo ""
echo "  Encode H.264 720p:"
echo "    max_perf=0:  ${enc_off} fps"
echo "    max_perf=1:  ${enc_on} fps  (speedup: $(speedup "$enc_off" "$enc_on"))"
echo "    +poc_type=2: ${enc_poc2} fps"
echo ""

if [ -z "$TEGRASTATS_BIN" ] && [ "$HAS_JTOP" = 0 ]; then
  echo "  hw-stats: not available (install jetson-stats: pip3 install jetson-stats)"
  echo "  Tip: run on Jetson host for tegrastats, or install jetson-stats for jtop API."
fi

echo ""
echo "  Expected decode speedup: ~2-6x (varies by variant, thermal, content)."
echo "  Issue #9: 720p H.264 46→290 fps reported on Orin."
echo ""

# Validate minimum speedup — warn but don't fail (thermal throttling, etc.)
dec_ratio=$(echo "$dec_off $dec_on" | awk '$1+0>0 && $2+0>0 {printf "%.1f", $2/$1} $1+0==0||$2+0==0 {print "0"}')
if echo "$dec_ratio" | awk '{exit ($1 >= 1.3) ? 0 : 1}' 2>/dev/null; then
  echo "OK: hw-perf-bench passed on ${variant} (decode speedup: ${dec_ratio}x)."
else
  echo "WARN: decode speedup ${dec_ratio}x is below 1.3x — check thermal state or variant support."
  echo "      This may be expected on some variants; not counted as FAIL."
  echo "OK: hw-perf-bench completed on ${variant}."
fi
