#!/usr/bin/env bash
# Decoder lifecycle suite: guards that blocking-wait changes did not break
# normal (non-low-delay) decode paths.
set -eu
# shellcheck source=test/gen-samples.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gen-samples.sh"

variant="${JETSON_VARIANT:-unknown}"
echo "=== hw-decoder-lifecycle on variant: ${variant} ==="

[ -s "${SAMPLE_H264_720P}" ] || gen-sample-h264 "${SAMPLE_H264_720P}" 3

echo "== 1. normal decode still works (no low_delay) =="
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
  -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
  -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: normal decode timed out."
  exit 1
fi
if is_signal_rc "$rc"; then
  echo "  warn: normal decode signal $((rc-128)), retrying after 200 ms..."
  sleep 0.2
  rc=0
  out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
    -c:v h264_nvmpi -i "${SAMPLE_H264_720P}" \
    -f null - 2>&1) || rc=$?
  if is_signal_rc "$rc"; then
    echo "FAIL: normal decode confirmed crash (signal $((rc-128)))."
    echo "$out" | tail -15
    exit 1
  fi
fi
frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
if [ -z "$frames" ] || [ "$frames" -lt 10 ]; then
  echo "FAIL: expected >= 10 frames, got ${frames:-0}."
  echo "$out" | tail -15
  exit 1
fi
echo "   normal decode: ${frames} frames — OK"

echo "== 2. flush and reuse (seek mid-stream) =="
SAMPLE_SEEK="/tmp/nvmpi-sample-h264-lifecycle-seek.mp4"
[ -s "${SAMPLE_SEEK}" ] || ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 5 \
  -c:v libx264 -preset fast -g 30 "${SAMPLE_SEEK}"
rc=0
out=$(timeout -k 5 30 ffmpeg -y -hide_banner \
  -ss 2 -c:v h264_nvmpi -i "${SAMPLE_SEEK}" \
  -f null - 2>&1) || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: seek+decode timed out — flush may be broken."
  exit 1
fi
frames=$(echo "$out" | grep -oP 'frame=\s*\K[0-9]+' | tail -1)
if [ -z "$frames" ] || [ "$frames" -lt 10 ]; then
  echo "FAIL: expected >= 10 frames after seek, got ${frames:-0}."
  exit 1
fi
echo "   flush+reuse: ${frames} frames after seek — OK"

echo "== 3. EOS no hang (short file) =="
SAMPLE_SHORT="/tmp/nvmpi-sample-h264-short.mp4"
[ -s "${SAMPLE_SHORT}" ] || ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i testsrc2=s=1280x720:r=30 -t 0.3 \
  -c:v libx264 "${SAMPLE_SHORT}"
rc=0
timeout -k 5 15 ffmpeg -y -hide_banner -loglevel error \
  -c:v h264_nvmpi -i "${SAMPLE_SHORT}" \
  -f null - 2>/dev/null || rc=$?
if [ "$rc" -eq 124 ]; then
  echo "FAIL: short-file decode timed out — EOS not signalled."
  exit 1
fi
echo "   short-file EOS OK (rc=${rc})"

echo "OK: hw-decoder-lifecycle passed on ${variant}."
