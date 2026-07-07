#!/usr/bin/env bash
# Encoder non-blocking mode suite: -nonblocking 1 makes put_frame return
# EAGAIN instead of blocking when no OUTPUT-plane buffer is available.
# FFmpeg's encode loop handles the retry internally.
#
# Assertions:
#   1. Non-blocking mode produces the same packet count as blocking mode.
#   2. Both H.264 and HEVC work in non-blocking mode.
#   3. Non-blocking + low_delay combination works.
#   4. Early termination with non-blocking exits cleanly (no hang).
#   5. Performance comparison: non-blocking should not be slower.
set -eu

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-encoder-nonblock on variant: ${variant} ==="

FRAMES=90
OUT_BLOCK=/tmp/nvmpi-out-nonblock-baseline.mp4
OUT_NOBLOCK=/tmp/nvmpi-out-nonblock-test.mp4

# Helper: encode and return packet count (or fail)
encode_check() {
  local enc="$1" flags="$2" outfile="$3" label="$4" expect="$5"
  local rc=0

  # shellcheck disable=SC2086
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -b:v 3M ${flags} \
    "$outfile" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: ${label} encode failed (rc=${rc})."
    exit 1
  fi

  local packets
  packets=$(ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 "$outfile")
  if [ "${packets:-0}" -ne "$expect" ]; then
    echo "FAIL: ${label} — expected ${expect} packets, got ${packets:-0}."
    exit 1
  fi
  echo "   ${label}: ${packets}/${expect} packets OK"
}

# Helper: timed encode, prints wall-clock seconds
timed_encode() {
  local enc="$1" flags="$2" outfile="$3"
  local start end

  start=$(date +%s%N)
  # shellcheck disable=SC2086
  ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i testsrc2=s=1280x720:r=30 \
    -frames:v ${FRAMES} -c:v "$enc" -b:v 3M ${flags} \
    "$outfile" 2>&1 || true
  end=$(date +%s%N)

  echo $(( (end - start) / 1000000 ))
}

# --- H.264: blocking baseline vs non-blocking ---

echo "== h264_nvmpi: blocking (default) =="
encode_check h264_nvmpi "" "$OUT_BLOCK" "h264 blocking" "$FRAMES"

echo "== h264_nvmpi: -nonblocking 1 =="
encode_check h264_nvmpi "-nonblocking 1" "$OUT_NOBLOCK" "h264 nonblocking" "$FRAMES"

# --- HEVC: blocking baseline vs non-blocking ---

echo "== hevc_nvmpi: blocking (default) =="
encode_check hevc_nvmpi "" "$OUT_BLOCK" "hevc blocking" "$FRAMES"

echo "== hevc_nvmpi: -nonblocking 1 =="
encode_check hevc_nvmpi "-nonblocking 1" "$OUT_NOBLOCK" "hevc nonblocking" "$FRAMES"

# --- Non-blocking + low_delay combination ---

echo "== h264_nvmpi: -nonblocking 1 -flags low_delay =="
encode_check h264_nvmpi "-nonblocking 1 -flags low_delay" "$OUT_NOBLOCK" \
  "h264 nonblocking+low_delay" "$FRAMES"

# --- Early termination with non-blocking (clean shutdown, no hang) ---

echo "== h264_nvmpi: -nonblocking 1 -t 1 (early termination) =="
timeout 30 ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 \
  -t 1 -c:v h264_nvmpi -b:v 3M -nonblocking 1 \
  "$OUT_NOBLOCK" 2>&1 || true

packets=$(ffprobe -v error -count_packets -select_streams v:0 \
  -show_entries stream=nb_read_packets -of default=nokey=1:nw=1 \
  "$OUT_NOBLOCK" 2>/dev/null || echo "0")
if [ "${packets:-0}" -gt 0 ]; then
  echo "   h264 early-term nonblocking: ${packets} packets, clean exit OK"
else
  echo "FAIL: h264 early-term nonblocking produced 0 packets or hung."
  exit 1
fi

# --- Performance comparison ---

echo "== Performance comparison (h264, ${FRAMES} frames) =="
ms_block=$(timed_encode h264_nvmpi "" "$OUT_BLOCK")
ms_noblock=$(timed_encode h264_nvmpi "-nonblocking 1" "$OUT_NOBLOCK")
echo "   blocking:     ${ms_block}ms"
echo "   non-blocking: ${ms_noblock}ms"

# Non-blocking should not be significantly slower (allow 20% margin for
# variance). This is informational — don't fail on timing jitter.
if [ "$ms_block" -gt 0 ]; then
  threshold=$(( ms_block * 120 / 100 ))
  if [ "$ms_noblock" -gt "$threshold" ]; then
    echo "   WARN: non-blocking was >20% slower — check for regression"
  else
    echo "   performance: within expected range"
  fi
fi

rm -f "$OUT_BLOCK" "$OUT_NOBLOCK"
echo "OK: hw-encoder-nonblock passed on ${variant}."
