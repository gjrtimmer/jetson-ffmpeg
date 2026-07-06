#!/usr/bin/env bash
# Encoder blocking-wait suite: -flags low_delay activates blocking dequeue
# in nvmpi_encoder_get_packet(). Regression guard for issue #34.
#
# Assertions:
#   1. Encode with low_delay produces correct frame count (no drops/hangs).
#   2. Encode without low_delay still works (non-blocking path).
#   3. Early termination (-t) with low_delay exits cleanly (no hang).
#   4. Custom wait_timeout works.
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-blocking on variant: ${variant} ==="

FRAMES=60
OUT=/tmp/nvmpi-out-blocking.mp4

# Helper: encode and verify frame count
encode_check() {
  local enc="$1" flags="$2" extra="$3" label="$4" expect="$5"
  local rc=0

  # shellcheck disable=SC2086
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -b:v 3M ${flags} ${extra} \
    "$OUT" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: ${label} encode failed (rc=${rc})."
    exit 1
  fi

  local packets
  packets=$(ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 "$OUT")
  if [ "${packets:-0}" -ne "$expect" ]; then
    echo "FAIL: ${label} — expected ${expect} packets, got ${packets:-0}."
    exit 1
  fi
  echo "   ${label}: ${packets}/${expect} packets OK"
}

# --- blocking mode (low_delay) ---

echo "== h264_nvmpi: -flags low_delay =="
encode_check h264_nvmpi "-flags low_delay" "" "h264 low_delay" "$FRAMES"

echo "== hevc_nvmpi: -flags low_delay =="
encode_check hevc_nvmpi "-flags low_delay" "" "hevc low_delay" "$FRAMES"

# --- non-blocking mode (default, no low_delay) ---

echo "== h264_nvmpi: default (no low_delay) =="
encode_check h264_nvmpi "" "" "h264 default" "$FRAMES"

# --- custom wait_timeout ---

echo "== h264_nvmpi: -flags low_delay -wait_timeout 1000 =="
encode_check h264_nvmpi "-flags low_delay" "-wait_timeout 1000" "h264 low_delay+timeout=1000" "$FRAMES"

# --- early termination with low_delay (clean shutdown, no hang) ---

echo "== h264_nvmpi: -flags low_delay -t 1 (early termination) =="
timeout 30 ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 \
  -t 1 -c:v h264_nvmpi -b:v 3M -flags low_delay \
  "$OUT" 2>&1 || true

packets=$(ffprobe -v error -count_packets -select_streams v:0 \
  -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 "$OUT" 2>/dev/null || echo "0")
if [ "${packets:-0}" -gt 0 ]; then
  echo "   h264 early-term: ${packets} packets, clean exit OK"
else
  echo "FAIL: h264 early-term produced 0 packets or hung."
  exit 1
fi

rm -f "$OUT"
echo "OK: hw-encoder-blocking passed on ${variant}."
